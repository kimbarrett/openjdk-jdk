/*
 * Copyright (c) 2014, 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/altHashing.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/oopStorage.hpp"
#include "gc/shared/oopStorageSet.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/stringdedup/stringDedupConfig.hpp"
#include "gc/shared/stringdedup/stringDedupStat.hpp"
#include "gc/shared/stringdedup/stringDedupTable.hpp"
#include "gc/shared/stringdedup/stringDedupTableValue.inline.hpp"
#include "logging/log.hpp"
#include "oops/access.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/hashtable.inline.hpp"
#include "utilities/macros.hpp"

//////////////////////////////////////////////////////////////////////////////
// StringDedup::Table::Entry

class StringDedup::Table::Entry : public HashtableEntry<TableValue, mtGC> {
  using Base = HashtableEntry<TableValue, mtGC>;

public:
  Entry* next() const {
    return static_cast<Entry*>(Base::next());
  }

  bool is_empty() const { return literal().is_empty(); }
  bool is_latin1() const { return literal().is_latin1(); }

  typeArrayOop object() const { return literal().resolve(); }
  typeArrayOop object_no_keepalive() const { return literal().peek(); }

  void release(OopStorage* storage) {
    literal().release(storage);
    set_literal(TableValue());
  }
};

//////////////////////////////////////////////////////////////////////////////
// Tracking dead entries
//
// Keeping accurate track of the number of dead entries in a table is
// complicated by the possibility that a GC could be changing the set while
// we're removing dead entries here, either atomically (STW reference
// processing) or even concurrently.
//
// If a dead count report is received while cleaning, further cleaning may
// reduce the number of dead entries.  With STW reference processing one can
// maintain an accurate dead count by deducting cleaned entries.  But that
// doesn't work for concurrent reference processsing.  In that case the dead
// count being reported may include entries that have already been removed
// by concurrent cleaning.
//
// It seems worse to unnecessarily resize or clean than to delay either.  So
// we track whether the reported dead count is good, and only consider
// resizing or cleaning when we have a good idea of the benefit.

enum class DeadState {
  // This is the initial state.  This state is also selected when a dead
  // count report is received and the state is wait1.  The reported dead
  // count is considered good.  It might be lower than actual because of an
  // in-progress concurrent reference processing.  It might also increase
  // immediately due to a new GC.  Oh well to both of those.
  good,
  // This state is selected when a dead count report is received and the
  // state is wait2.  Current value of dead count may be inaccurate because
  // of reference processing that was started before or during the most
  // recent cleaning and finished after.  Wait for the next report.
  wait1,
  // This state is selected when a resize or cleaning operation completes.
  // Current value of dead count is inaccurate because we haven't had a
  // report since the last cleaning.
  wait2,
  // Currently resizing or cleaning the table.
  cleaning
};

static const char* dead_state_name(DeadState state) {
  const char* result = "unknown";
  switch (state) {
  case DeadState::good: result = "good"; break;
  case DeadState::wait1: result = "wait1"; break;
  case DeadState::wait2: result = "wait2"; break;
  case DeadState::cleaning: result = "cleaning"; break;
  }
  return result;
}

//////////////////////////////////////////////////////////////////////////////
// StringDedup::Table::HTable

class StringDedup::Table::HTable : public Hashtable<TableValue, mtGC> {
  using Base = Hashtable<TableValue, mtGC>;

  Entry** bucket_addr(int i) {
    return reinterpret_cast<Entry**>(Base::bucket_addr(i));
  }

  Entry* new_entry(uint hash, oop* array_ref, bool latin1);
  Entry* find(typeArrayOop obj, bool latin1, uint hash) const;
  NONCOPYABLE(HTable);

  OopStorage* _storage;
  uint64_t _hash_seed;
  size_t _dead_count;
  DeadState _dead_state;

public:
  HTable(OopStorage* storage, size_t size, uint64_t hash_seed);
  ~HTable();

  OopStorage* storage() const { return _storage; }
  uint64_t hash_seed() const { return _hash_seed; }
  void notify_dead(size_t num_dead);
  void record_cleanup_start();
  void record_cleanup_end();

  uint compute_hash(typeArrayOop obj, bool latin1) const;

  bool add_shared_string(oop* shared_entry, Stat* stat);
  typeArrayOop intern(typeArrayOop obj, bool latin1, Stat* stat);

  size_t dead_count() const {
    assert_lock_strong(StringDedup_lock);
    return _dead_count;
  }

  DeadState dead_state() const {
    assert_lock_strong(StringDedup_lock);
    return _dead_state;
  }

  Entry* pop_bucket(int index);
  Entry* bucket(int i) const {
    return static_cast<Entry*>(Base::bucket(i));
  }

  using Base::unlink_entry;

  // precondition: entry must not have been inserted.
  void insert(Entry* entry);

  // precondition: entry must have been unlinked.
  void free_entry(Entry* entry);

  void verify() const;
};

StringDedup::Table::HTable::HTable(OopStorage* storage,
                                   size_t size,
                                   uint64_t hash_seed) :
  Base(checked_cast<int>(size), sizeof(Entry)),
  _storage(storage),
  _hash_seed(hash_seed),
  _dead_count(0),
  _dead_state(DeadState::good)
{}

StringDedup::Table::HTable::~HTable() {
  Unimplemented();
}

StringDedup::Table::Entry*
StringDedup::Table::HTable::new_entry(uint hash, oop* array_ref, bool latin1) {
  assert(_storage->allocation_status(array_ref) == OopStorage::ALLOCATED_ENTRY, "precondition");
  TableValue value{array_ref, latin1};
  return static_cast<Entry*>(Base::new_entry(hash, value));
}

// Compute the hash code for obj+latin1 using halfsiphash_32.  As this is a
// high quality hash function that is resistant to hashtable flooding, long
// bucket chains should be very rare.  Bucket searches are interruptable, so
// long bucket chains are not a latency problem.  And table lookup is not
// really system performance critical. As a result of all this, we don't
// bother monitoring for long chains and triggering rehashes in response.
uint StringDedup::Table::HTable::compute_hash(typeArrayOop obj, bool latin1) const {
  int length = obj->length();
  uint hash;
  if (latin1) {
    const uint8_t* data = static_cast<uint8_t*>(obj->base(T_BYTE));
    hash = AltHashing::halfsiphash_32(_hash_seed, data, length);
  } else {
    const uint16_t* data = static_cast<uint16_t*>(obj->base(T_CHAR));
    hash = AltHashing::halfsiphash_32(_hash_seed, data, length);
  }
  return hash;
}

StringDedup::Table::Entry*
StringDedup::Table::HTable::find(typeArrayOop obj, bool latin1, uint hash) const {
  int index = hash_to_index(hash);
  for (Entry* entry = bucket(index); entry != nullptr; entry = entry->next()) {
    if ((entry->hash() == hash) && (entry->is_latin1() == latin1)) {
      // Check for cleared entries, but don't bother removing them.
      // Doing so adds complexity for little gain; with a good hash function
      // we should rarely need to look at a non-matching entry.
      typeArrayOop entry_value = entry->object_no_keepalive();
      if ((entry_value != nullptr) &&
          (java_lang_String::value_equals(obj, entry_value)))
        return entry;
    }
  }
  return nullptr;
}

StringDedup::Table::Entry*
StringDedup::Table::HTable::pop_bucket(int index) {
  // Avoid set_entry(index, next), which generates a JFR remove event that
  // we don't want here.  We're either popping a good element as part of
  // rebuild, or popping a dead element as part of cleaning.  In the former
  // we're not really doing a remove. In the latter free_entry also
  // generates a JFR remove event.
  Entry* entry = static_cast<Entry*>(*this->bucket_addr(index));
  if (entry != nullptr) {
    *this->bucket_addr(index) = entry->next();
  }
  return entry;
}

bool StringDedup::Table::HTable::add_shared_string(oop* string_ref, Stat* stat) {
  oop obj = NativeAccess<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>::oop_load(string_ref);
  if (obj == nullptr) {
    stat->inc_skipped();
    return false;
  }
  assert(java_lang_String::is_instance(obj), "precondition");
  typeArrayOop value = java_lang_String::value(obj);
  bool latin1 = java_lang_String::is_latin1(obj);
  int hash = compute_hash(value, latin1);
  Entry* entry = find(value, latin1, hash);
  if (entry != nullptr) {
    stat->inc_known();
    return false;
  }
  // Replace the string with its data array in the oopstorage ref.
  NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(string_ref, value);
  entry = new_entry(hash, string_ref, latin1);
  insert(entry);
  stat->inc_new(value->size() * HeapWordSize);
  return true;
}

typeArrayOop StringDedup::Table::HTable::intern(typeArrayOop obj,
                                                bool latin1,
                                                Stat* stat) {
  assert(obj != nullptr, "precondition");
  uint hash = compute_hash(obj, latin1);
  Entry* entry = find(obj, latin1, hash);
  if (entry != nullptr) {
    stat->inc_known();
  } else {
    // Not already in table.  Try to allocate a new table entry.
    oop* array_ref = _storage->allocate();
    if (array_ref == nullptr) {
      // Allocation of oopstorage entry failed; just drop the request.
      stat->inc_skipped();
      return nullptr;
    }
    NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(array_ref, obj);
    entry = new_entry(hash, array_ref, latin1);
    insert(entry);
    stat->inc_new(obj->size() * HeapWordSize);
  }
  typeArrayOop result = entry->object();
  assert(result != nullptr, "invariant");
  return result;
}

void StringDedup::Table::HTable::insert(Entry* entry) {
  int index = hash_to_index(entry->hash());
  Base::add_entry(index, entry);
}

void StringDedup::Table::HTable::free_entry(Entry* entry) {
  assert(entry->next() == nullptr, "precondition");
  entry->release(_storage);
  Base::free_entry(entry);
}

void StringDedup::Table::HTable::notify_dead(size_t num_dead) {
  assert_lock_strong(StringDedup_lock);
  switch (_dead_state) {
  case DeadState::wait1:
    _dead_state = DeadState::good;
    // fallthrough
  case DeadState::good:
    _dead_count = num_dead;
    break;

  case DeadState::wait2:
    _dead_state = DeadState::wait1;
    break;

  case DeadState::cleaning:
    break;
  }
}

void StringDedup::Table::HTable::record_cleanup_start() {
  assert_lock_strong(StringDedup_lock);
  _dead_count = 0;
  _dead_state = DeadState::cleaning;
}

void StringDedup::Table::HTable::record_cleanup_end() {
  assert_lock_strong(StringDedup_lock);
  _dead_state = DeadState::wait2;
}

void StringDedup::Table::HTable::verify() const {
  int element_count = 0;
  for (int i = 0; i < table_size(); ++i) {
    for (const Entry* entry = bucket(i); entry != nullptr; entry = entry->next()) {
      ++element_count;
      guarantee(i == hash_to_index(entry->hash()),
                "entry in wrong bucket: " PTR_FORMAT ": %d", p2i(entry), i);
      guarantee(!entry->is_empty(),
                "entry missing value: " PTR_FORMAT ": %d", p2i(entry), i);
      const oop* value = entry->literal().storage_entry();
      OopStorage::EntryStatus status = _storage->allocation_status(value);
      guarantee(OopStorage::ALLOCATED_ENTRY == status,
                "bad entry value: " PTR_FORMAT " -> " PTR_FORMAT " (%d)",
                p2i(entry), p2i(value), status);
      // Don't check object is oop_or_null; duplicates OopStorage verify.
    }
  }
  guarantee(element_count == number_of_entries(),
            "number of entries mismatch: %d counted, %d recorded",
            element_count, number_of_entries());
}

//////////////////////////////////////////////////////////////////////////////
// StringDedup::Table::RebuildState

class StringDedup::Table::RebuildState : public CHeapObj<mtGC> {
  HTable* _table;
  Entry* _entries;
  Stat* _stat;
  int _new_size;
  int _index;
  bool _collecting;

  NONCOPYABLE(RebuildState);

public:
  RebuildState(HTable* table, int new_size, Stat* stat) :
    _table(table),
    _entries(nullptr),
    _stat(stat),
    _new_size(new_size),
    _index(0),
    _collecting(true)
  {}

  HTable* table() const { return _table; }
  Stat* stat() const { return _stat; }

  bool step();
  void abandon();
};

bool StringDedup::Table::RebuildState::step() {
  // Collect entries.
  if (_collecting && (_index < _table->table_size())) {
    Entry* entry = _table->pop_bucket(_index);
    if (entry == nullptr) {
      ++_index;
    } else {
      _table->unlink_entry(entry);
      entry->set_next(_entries);
      _entries = entry;
    }
    return true;               // End step, with more steps remaining.
  }
  // Done collecting entries.  Update table for entry reinsertion.
  if (_collecting) {
    assert(_table->number_of_entries() == 0, "invariant");
    _collecting = false;
    if (!_table->resize(_new_size)) {
      log_warning(gc, stringdedup)("Allocation failed for resized table. Abandoning data.");
      abandon();
      return false;
    }
    return true;                // End step, with more steps remaining.
  }
  // Reinsert live entries and free dead entries.
  if (_entries != nullptr) {
    // Pop next entry from _entries.
    Entry* entry = _entries;
    _entries = entry->next();
    entry->set_next(nullptr);
    // Unconditionally insert the entry, then conditionally remove and free
    // if dead.  This clumsy little dance is needed to maintain the number
    // of entries, since free_entry decrements it.
    _table->insert(entry);
    if (entry->object_no_keepalive() == nullptr) {
      Entry* popped = _table->pop_bucket(_table->hash_to_index(entry->hash()));
      assert(popped == entry, "invariant");
      DEBUG_ONLY(entry->set_next(nullptr);)
      _table->free_entry(entry);
      _stat->inc_deleted();
    }
    return true;                // End step, with more steps remaining.
  }
  return false;                 // No more steps.
}

void StringDedup::Table::RebuildState::abandon() {
  while (_entries != nullptr) {
    Entry* entry = _entries;
    _entries = entry->next();
    entry->set_next(nullptr);
    _table->free_entry(entry);
  }
}

//////////////////////////////////////////////////////////////////////////////
// StringDedup::Table::CleanupState

class StringDedup::Table::CleanupState : public CHeapObj<mtGC> {
  HTable* _table;
  Stat* _stat;
  Entry* _prev_entry;
  int _index;

  NONCOPYABLE(CleanupState);

public:
  CleanupState(HTable* table, Stat* stat) :
    _table(table),
    _stat(stat),
    _prev_entry(nullptr),
    _index(0)
  {}

  HTable* table() const { return _table; }
  Stat* stat() const { return _stat; }

  bool step();
};

bool StringDedup::Table::CleanupState::step() {
  if (_index >= _table->table_size()) return false;
  // Get the next entry from the current bucket.
  Entry* entry;
  if (_prev_entry == nullptr) {
    entry = _table->bucket(_index);
  } else {
    entry = _prev_entry->next();
  }
  if (entry == nullptr) {
    // No more entries in bucket. Step to next bucket.
    _prev_entry = nullptr;
    ++_index;
  } else if (entry->object_no_keepalive() != nullptr) {
    // Entry has live value. Step to next entry.
    _prev_entry = entry;
  } else {
    // Unlink and free entry with dead value.
    if (_prev_entry == nullptr) {
      _table->pop_bucket(_index);
    } else {
      _prev_entry->set_next(entry->next());
    }
    DEBUG_ONLY(entry->set_next(nullptr);)
    _table->free_entry(entry);
    _stat->inc_deleted();
  }
  return true;                  // End step, with more steps remaining.
}

//////////////////////////////////////////////////////////////////////////////
// StringDedup::Table

StringDedup::Table::Table() :
  _ht(new HTable(_table_storage,
                 _config->initial_table_size(),
                 _config->hash_seed()))
{}

StringDedup::Table::~Table() {
  Unimplemented();
}

OopStorage* StringDedup::Table::_table_storage = nullptr;

void StringDedup::Table::initialize_storage() {
  assert(_table_storage == nullptr, "storage already created");
  _table_storage = OopStorageSet::create_weak("StringDedup Table Weak");
}

void StringDedup::Table::num_dead_callback(size_t num_dead) {
  MonitorLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
  _table->_ht->notify_dead(num_dead);
  ml.notify_all();
}

void StringDedup::Table::initialize() {
  _table = new Table();
  _table->storage()->register_num_dead_callback(num_dead_callback);
}

void StringDedup::Table::destroy() {
  delete _table;
  _table = nullptr;
}

OopStorage* StringDedup::Table::storage() const {
  return _ht->storage();
}

bool StringDedup::Table::add_shared_string(oop* string_ref, Stat* stat) {
  return _ht->add_shared_string(string_ref, stat);
}

void StringDedup::Table::deduplicate(oop java_string, Stat* stat) {
  assert(java_lang_String::is_instance(java_string), "precondition");
  typeArrayOop value = java_lang_String::value(java_string);
  if (value == nullptr) {
    stat->inc_skipped();
  } else {
    bool latin1 = java_lang_String::is_latin1(java_string);
    typeArrayOop ivalue = _ht->intern(value, latin1, stat);
    if ((ivalue != value) && (ivalue != nullptr)) {
      java_lang_String::set_value(java_string, ivalue);
      stat->inc_deduped(value->size() * HeapWordSize);
    }
  }
}

StringDedup::Table::RebuildState* StringDedup::Table::rebuild_start(Stat* stat) {
  int table_size = _ht->table_size();
  int new_size = table_size;    // Default to unchanged.
  size_t entry_count = _ht->number_of_entries();
  size_t dead_count;
  {
    // Lock out num_dead updates while computing desired size.
    MutexLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
    // Don't consider resize when dead count isn't known good.
    if (_ht->dead_state() != DeadState::good) return nullptr;
    // Based on the number of live entries, decide whether to try to resize
    // the table, and if so, what the new size should be.
    dead_count = _ht->dead_count();
    assert(dead_count <= entry_count, "invariant");
    size_t adj_count = entry_count - dead_count;
    if (_config->should_grow_table(table_size, adj_count) ||
        _config->should_shrink_table(table_size, adj_count)) {
      new_size = _config->desired_table_size(adj_count);
    }
    // Don't start an unnecessary resize.
    if ((new_size == table_size) && !StringDeduplicationResizeALot) {
      return nullptr;
    }
    _ht->record_cleanup_start(); // Record while still holding lock.
  } // Drop lock before constructing state.
  stat->report_rebuild_table_start(new_size, table_size, entry_count, dead_count);
  return new RebuildState(_ht, new_size, stat);
}

bool StringDedup::Table::rebuild_step(RebuildState* state) {
  return state->step();
}

void StringDedup::Table::rebuild_end(RebuildState* state) {
  state->abandon();             // If ending early, abandon partial state.
  HTable* table = state->table();
  Stat* stat = state->stat();
  delete state;
  {
    MutexLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
    table->record_cleanup_end();
  }
  stat->report_rebuild_table_end();
}

StringDedup::Table::CleanupState* StringDedup::Table::cleanup_start(Stat* stat) {
  size_t entry_count = _ht->number_of_entries();
  size_t dead_count;
  {
    // Lock out num_dead update while testing and starting cleanup.
    MutexLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
    // Don't consider cleaning when dead count isn't known good.
    if (_ht->dead_state() != DeadState::good) return nullptr;
    dead_count = _ht->dead_count();
    if (!_config->should_cleanup_table(entry_count, dead_count)) {
      return nullptr;
    }
    _ht->record_cleanup_start(); // Record while still holding lock.
  } // Drop lock before constructing state.
  stat->report_cleanup_table_start(entry_count, dead_count);
  return new CleanupState(_ht, stat);
}

bool StringDedup::Table::cleanup_step(CleanupState* state) {
  return state->step();
}

void StringDedup::Table::cleanup_end(CleanupState* state) {
  HTable* table = state->table();
  Stat* stat = state->stat();
  delete state;
  {
    MutexLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
    table->record_cleanup_end();
  }
  stat->report_cleanup_table_end();
}

void StringDedup::Table::verify() const {
  _ht->verify();
}

void StringDedup::Table::log_statistics() const {
  // Lock required for accessing dead count and state.
  MutexLocker ml(StringDedup_lock, Mutex::_no_safepoint_check_flag);
  log_debug(gc, stringdedup)("Table: %d entries in %d slots, %zu dead (%s)",
                             _ht->number_of_entries(),
                             _ht->table_size(),
                             _ht->dead_count(),
                             dead_state_name(_ht->dead_state()));
}
