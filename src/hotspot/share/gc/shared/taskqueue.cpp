/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/freeListAllocator.hpp"
#include "gc/shared/taskqueue.hpp"
#include "oops/oop.inline.hpp"
#include "logging/log.hpp"
#include "memory/arena.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutex.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/stack.inline.hpp"
#include <new>

#if TASKQUEUE_STATS
const char * const TaskQueueStats::_names[last_stat_id] = {
  "push", "pop", "pop-slow",
  "st-attempt", "st-empty", "st-ctdd", "st-success", "st-ctdd-max", "st-biasdrop",
  "ovflw-push", "ovflw-max"
};

TaskQueueStats & TaskQueueStats::operator +=(const TaskQueueStats & addend)
{
  for (unsigned int i = 0; i < last_stat_id; ++i) {
    _stats[i] += addend._stats[i];
  }
  return *this;
}

void TaskQueueStats::print_header(unsigned int line, outputStream* const stream,
                                  unsigned int width)
{
  // Use a width w: 1 <= w <= max_width
  const unsigned int max_width = 40;
  const unsigned int w = clamp(width, 1u, max_width);

  if (line == 0) { // spaces equal in width to the header
    const unsigned int hdr_width = w * last_stat_id + last_stat_id - 1;
    stream->print("%*s", hdr_width, " ");
  } else if (line == 1) { // labels
    stream->print("%*s", w, _names[0]);
    for (unsigned int i = 1; i < last_stat_id; ++i) {
      stream->print(" %*s", w, _names[i]);
    }
  } else if (line == 2) { // dashed lines
    char dashes[max_width + 1];
    memset(dashes, '-', w);
    dashes[w] = '\0';
    stream->print("%s", dashes);
    for (unsigned int i = 1; i < last_stat_id; ++i) {
      stream->print(" %s", dashes);
    }
  }
}

void TaskQueueStats::print(outputStream* stream, unsigned int width) const
{
  #define FMT SIZE_FORMAT_W(*)
  stream->print(FMT, width, _stats[0]);
  for (unsigned int i = 1; i < last_stat_id; ++i) {
    stream->print(" " FMT, width, _stats[i]);
  }
  #undef FMT
}

#ifdef ASSERT
// Invariants which should hold after a TaskQueue has been emptied and is
// quiescent; they do not hold at arbitrary times.
void TaskQueueStats::verify() const
{
  assert(get(push) == get(pop) + get(steal_success),
         "push=%zu pop=%zu steal=%zu",
         get(push), get(pop), get(steal_success));
  assert(get(pop_slow) <= get(pop),
         "pop_slow=%zu pop=%zu",
         get(pop_slow), get(pop));
  assert(get(steal_empty) <= get(steal_attempt),
         "steal_empty=%zu steal_attempt=%zu",
         get(steal_empty), get(steal_attempt));
  assert(get(steal_contended) <= get(steal_attempt),
         "steal_contended=%zu steal_attempt=%zu",
         get(steal_contended), get(steal_attempt));
  assert(get(steal_success) <= get(steal_attempt),
         "steal_success=%zu steal_attempt=%zu",
         get(steal_success), get(steal_attempt));
  assert(get(steal_empty) + get(steal_contended) + get(steal_success) == get(steal_attempt),
         "steal_empty=%zu steal_contended=%zu steal_success=%zu steal_attempt=%zu",
         get(steal_empty), get(steal_contended), get(steal_success), get(steal_attempt));
  assert(get(overflow) == 0 || get(push) != 0,
         "overflow=%zu push=%zu",
         get(overflow), get(push));
  assert(get(overflow_max_len) == 0 || get(overflow) != 0,
         "overflow_max_len=%zu overflow=%zu",
         get(overflow_max_len), get(overflow));
}
#endif // ASSERT
#endif // TASKQUEUE_STATS

#ifdef ASSERT
bool ObjArrayTask::is_valid() const {
  return _obj != nullptr && _obj->is_objArray() && _index >= 0 &&
      _index < objArrayOop(_obj)->length();
}
#endif // ASSERT

class ObjArrayScanState::Allocator::Config : public FreeListConfig {
  Mutex* _mutex;
  Arena _arena;

public:
  Config(Mutex* mutex);
  void* allocate() override;
  void deallocate(void* node) override;
};

ObjArrayScanState::Allocator::Config::Config(Mutex* mutex)
  : FreeListConfig(100),     // FIXME: configurable transfer threshold
    _mutex(mutex),
    _arena(mtGC)
{}

void* ObjArrayScanState::Allocator::Config::allocate() {
  MutexLocker ml(_mutex, Mutex::_no_safepoint_check_flag);
  return NEW_ARENA_OBJ(&_arena, ObjArrayScanState);
}

void ObjArrayScanState::Allocator::Config::deallocate(void* node) {
  // Do nothing, since we're arena allocating.
  // Arenas can undo the last allocation, but since we're using a free-list
  // allocator in front of the arena, that's not useful.
}

class ObjArrayScanState::Allocator::Impl : public CHeapObj<mtGC> {
  Mutex _mutex;                 // FIXME should come from outside
  Config _config;
  FreeListAllocator _free_list;

public:
  Impl(const char* name);
  ~Impl();

  NONCOPYABLE(Impl);

  ObjArrayScanState* allocate(oop src, oop dst, int index, int size);
  void release(ObjArrayScanState* state);
};

ObjArrayScanState::Allocator::Impl::Impl(const char* name)
  : _mutex(Mutex::nosafepoint, name),
    _config(&_mutex),
    _free_list(name, &_config)
{}

ObjArrayScanState::Allocator::Impl::~Impl() {
  // We don't need to clean up the free list.  Deallocating the entries
  // does nothing, since we're using arena allocation.  Instead, leave it
  // to the arena destructor to release the memory.
  _free_list.reset();
}

ObjArrayScanState* ObjArrayScanState::Allocator::Impl::allocate(oop src, oop dst, int index, int size) {
  void* p = _free_list.allocate();
  return ::new (p) ObjArrayScanState(src, dst, index, size);
}

void ObjArrayScanState::Allocator::Impl::release(ObjArrayScanState* state) {
  state->~ObjArrayScanState();
  _free_list.release(state);
}

ObjArrayScanState::Allocator::Allocator()
  : _impl(new Impl("ObjArrayScanState allocator"))
{}

ObjArrayScanState::Allocator::~Allocator() {
  delete _impl;
}

ObjArrayScanState* ObjArrayScanState::Allocator::allocate(oop src, oop dst, int index, int size) {
  return _impl->allocate(src, dst, index, size);
}

void ObjArrayScanState::Allocator::release(ObjArrayScanState* state) {
  _impl->release(state);
}

ObjArrayScanState::ObjArrayScanState(oop src, oop dst, int index, int size)
  : _source(src),
    _destination(dst),
    _size(size),
    _index(index),
    _refcount(0)
{
  assert(index < size, "precondition");
}

void ObjArrayScanState::add_references(uint count) {
  uint new_count = Atomic::add(&_refcount, count, memory_order_relaxed);
  assert(new_count >= count, "reference count overflow");
}

bool ObjArrayScanState::release_reference() {
  uint new_count = Atomic::sub(&_refcount, 1u);
  assert(new_count + 1 != 0, "reference count underflow");
  return new_count == 0;
}
