/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/stringTable.hpp"
#include "gc/shared/oopStorage.hpp"
#include "gc/shared/oopStorageParState.inline.hpp"
#include "gc/shared/oopStorageSet.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/stringdedup/stringDedupProcessor.hpp"
#include "gc/shared/stringdedup/stringDedupStat.hpp"
#include "gc/shared/stringdedup/stringDedupStorageUse.hpp"
#include "gc/shared/stringdedup/stringDedupTable.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "oops/access.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalCounter.hpp"
#include "utilities/globalDefinitions.hpp"

StringDedup::Processor::Processor() : ConcurrentGCThread() {
  set_name("StringDedupProcessor");
}

StringDedup::Processor::~Processor() {}

OopStorage* StringDedup::Processor::_storages[2] = {};

StringDedup::StorageUse* volatile StringDedup::Processor::_storage_for_requests = nullptr;
StringDedup::StorageUse* StringDedup::Processor::_storage_for_processing = nullptr;

void StringDedup::Processor::initialize_storage() {
  assert(_storages[0] == nullptr, "storage already created");
  assert(_storages[1] == nullptr, "storage already created");
  assert(_storage_for_requests == nullptr, "storage already created");
  assert(_storage_for_processing == nullptr, "storage already created");
  _storages[0] = OopStorageSet::create_weak("StringDedup Requests0 Weak");
  _storages[1] = OopStorageSet::create_weak("StringDedup Requests1 Weak");
  _storage_for_requests = new StorageUse(_storages[0]);
  _storage_for_processing = new StorageUse(_storages[1]);
}

void StringDedup::Processor::initialize() {
  _processor = new Processor();
  _processor->create_and_start();
}

void StringDedup::Processor::destroy() {
  assert(_processor->has_terminated(), "unterminated thread %s", _processor->name());
  delete _processor;
  _processor = nullptr;
}

bool StringDedup::Processor::wait_for_requests() const {
  // Wait for the current request storage object to be non-empty.  The
  // num-dead notification from the Table notifies the monitor.
  if (!should_terminate()) {
    MonitorLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
    OopStorage* storage = Atomic::load(&_storage_for_requests)->storage();
    while ((storage->allocation_count() == 0) && !should_terminate()) {
      ml.wait();
    }
  }
  // Swap the request and processing storage objects.
  if (!should_terminate()) {
    log_trace(gc, stringdedup)("swapping request storages");
    _storage_for_processing = Atomic::xchg(&_storage_for_requests, _storage_for_processing);
    GlobalCounter::write_synchronize();
  }
  // Wait for the now current processing storage object to no longer be used
  // by an in-progress GC.  Again here, the num-dead notification from the
  // Table notifies the monitor.
  if (!should_terminate()) {
    log_trace(gc, stringdedup)("waiting for storage to process");
    MonitorLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
    while (_storage_for_processing->is_used_acquire() && !should_terminate()) {
      ml.wait();
    }
  }
  return !should_terminate();
}

StringDedup::StorageUse* StringDedup::Processor::storage_for_requests() {
  return StorageUse::obtain(&_storage_for_requests);
}

bool StringDedup::Processor::yield_or_continue(SuspendibleThreadSetJoiner* joiner,
                                               Stat* stat,
                                               Stat::Phase phase) const {
  if (joiner->should_yield()) {
    stat->block_phase(phase);
    joiner->yield();
    stat->unblock_phase();
  }
  return !should_terminate();
}

bool StringDedup::Processor::rebuild_table(SuspendibleThreadSetJoiner* joiner,
                                           Stat* stat) const {
  Table::RebuildState* state = _table->rebuild_start(stat);
  if (state == nullptr) return false;
  while (_table->rebuild_step(state)) {
    if (!yield_or_continue(joiner, stat, Stat::Phase::rebuild_table)) {
      break;
    }
  }
  _table->rebuild_end(state);
  return true;
}

void StringDedup::Processor::cleanup_table(SuspendibleThreadSetJoiner* joiner,
                                           Stat* stat) const {
  Table::CleanupState* state = _table->cleanup_start(stat);
  if (state == nullptr) return;
  while (_table->cleanup_step(state)) {
    if (!yield_or_continue(joiner, stat, Stat::Phase::cleanup_table)) {
      break;
    }
  }
  _table->cleanup_end(state);
}

class StringDedup::Processor::CollectShared final : public ObjectClosure {
  oop* _collecting[OopStorage::bulk_allocate_limit];
  size_t _index;
  size_t _skipped;
  const Processor* _processor;
  OopStorage* _storage;

public:
  CollectShared(const Processor* processor, OopStorage* storage) :
    _collecting{}, _index(0), _skipped(0), _processor(processor), _storage(storage) {}

  ~CollectShared() {
    _storage->release(_collecting, _index);
  }

  size_t skipped() const { return _skipped; }

  virtual void do_object(oop obj) {
    if (_index == 0) {
      if ((_skipped > 0) || _processor->should_terminate()) {
        ++_skipped;
        return;
      }
      _index = _storage->allocate(_collecting, ARRAY_SIZE(_collecting));
      if (_index == 0) {
        _skipped = 1;
        return;
      }
    }
    oop* p = _collecting[--_index];
    _collecting[_index] = nullptr;
    NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(p, obj);
  }
};

class StringDedup::Processor::ProcessBase : public OopClosure {
  static const size_t _block_size = 50;

  const Processor* _processor;
  OopStorage* _storage;
  Table* _table;
  SuspendibleThreadSetJoiner* _joiner;
  Stat* _stat;
  oop* _bulk_release[_block_size];
  size_t _release_index;

protected:
  ProcessBase(const Processor* processor,
              OopStorage* storage,
              Table* table,
              SuspendibleThreadSetJoiner* joiner,
              Stat* stat) :
    _processor(processor),
    _storage(storage),
    _table(table),
    _joiner(joiner),
    _stat(stat),
    _bulk_release(),
    _release_index(0)
  {}

  ~ProcessBase() {
    _storage->release(_bulk_release, _release_index);
  }

  Table* table() const { return _table; }
  Stat* stat() const { return _stat; }

  virtual bool process(oop* ref) = 0;

public:
  virtual void do_oop(narrowOop*) { ShouldNotReachHere(); }

  virtual void do_oop(oop* ref) {
    if (_processor->yield_or_continue(_joiner, _stat, Stat::Phase::process) &&
        !process(ref)) {
      assert(_release_index < _block_size, "invariant");
      NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(ref, nullptr);
      _bulk_release[_release_index++] = ref;
      if (_release_index == _block_size) {
        _storage->release(_bulk_release, _release_index);
        _release_index = 0;
      }
    }
  }
};

class StringDedup::Processor::ProcessShared final : public ProcessBase {
protected:
  virtual bool process(oop* ref) {
    return table()->add_shared_string(ref, stat());
  }

public:
  ProcessShared(const Processor* processor,
                OopStorage* storage,
                Table* table,
                SuspendibleThreadSetJoiner* joiner,
                Stat* stat) :
    ProcessBase(processor, storage, table, joiner, stat)
  {}
};

class StringDedup::Processor::ProcessRequest final : public ProcessBase {
protected:
  virtual bool process(oop* ref) {
    oop java_string = NativeAccess<ON_PHANTOM_OOP_REF>::oop_load(ref);
    if (java_string != nullptr) {
      table()->deduplicate(java_string, stat());
    }
    return false;               // Always clear and release ref.
  }

public:
  ProcessRequest(const Processor* processor,
                 OopStorage* storage,
                 Table* table,
                 SuspendibleThreadSetJoiner* joiner,
                 Stat* stat) :
    ProcessBase(processor, storage, table, joiner, stat)
  {}
};

void StringDedup::Processor::collect_shared() const {
  CollectShared collector{this, _table->storage()};
  StringTable::shared_oops_do(&collector);
  if (collector.skipped() > 0) {
    log_warning(gc, stringdedup)("Skipped %zu shared strings", collector.skipped());
  }
}

void StringDedup::Processor::process_shared(SuspendibleThreadSetJoiner* joiner,
                                            Stat* stat) const {
  OopStorage::ParState<true, false> par_state{_table->storage(), 1};
  ProcessShared processor{this, _table->storage(), _table, joiner, stat};
  par_state.oops_do(&processor);
}

void StringDedup::Processor::process_requests(SuspendibleThreadSetJoiner* joiner,
                                              Stat* stat) const {
  OopStorage::ParState<true, false> par_state{_storage_for_processing->storage(), 1};
  ProcessRequest processor{this, _storage_for_processing->storage(), _table, joiner, stat};
  par_state.oops_do(&processor);
}

void StringDedup::Processor::run_service() {
  Stat total_stat{};

  {
    Stat stat{};
    stat.report_process_start();
    collect_shared();
    stat.report_process_end();
    total_stat.add(&stat);
    log_statistics(&stat, &total_stat);
  }

  // First iteration processes the shared requests collected above.
  // All later iterations wait for requests and process them.
  for (bool shared = true; !should_terminate(); /* */) {
    Stat stat{};
    stat.report_idle_start();
    if (!shared && !wait_for_requests()) {
      assert(should_terminate(), "invariant");
      break;
    }
    SuspendibleThreadSetJoiner sts_joiner{};
    if (should_terminate()) break;
    stat.report_idle_end();
    stat.report_concurrent_start();
    stat.report_process_start();
    if (shared) {
      process_shared(&sts_joiner, &stat);
      shared = false;
    } else {
      process_requests(&sts_joiner, &stat);
    }
    if (should_terminate()) break;
    stat.report_process_end();

    if (!rebuild_table(&sts_joiner, &stat)) {
      cleanup_table(&sts_joiner, &stat);
    }
    if (should_terminate()) break;
    stat.report_concurrent_end();
    total_stat.add(&stat);
    log_statistics(&stat, &total_stat);
  }
}

void StringDedup::Processor::stop_service() {
  MonitorLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
  ml.notify_all();
}

void StringDedup::Processor::log_statistics(const Stat* last_stat,
                                            const Stat* total_stat) const {
  Stat::log_summary(last_stat, total_stat);
  if (log_is_enabled(Debug, gc, stringdedup)) {
    last_stat->log_statistics(false);
    total_stat->log_statistics(true);
    _table->log_statistics();
  }
}
