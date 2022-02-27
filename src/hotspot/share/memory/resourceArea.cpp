/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.inline.hpp"
#include "nmt/memTracker.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/vmError.hpp"

void ResourceArea::bias_to(MEMFLAGS new_flags) {
  if (new_flags != _flags) {
    size_t size = size_in_bytes();
    MemTracker::record_arena_size_change(-ssize_t(size), _flags);
    MemTracker::record_arena_free(_flags);
    MemTracker::record_new_arena(new_flags);
    MemTracker::record_arena_size_change(ssize_t(size), new_flags);
    _flags = new_flags;
  }
}

#ifdef ASSERT

void ResourceArea::verify_has_resource_mark() {
  if (_nesting <= 0 && !VMError::is_error_reported()) {
    // Only report the first occurrence of an allocating thread that
    // is missing a ResourceMark, to avoid possible recursive errors
    // in error handling.
    static volatile bool reported = false;
    if (!Atomic::load(&reported)) {
      if (!Atomic::cmpxchg(&reported, false, true)) {
        fatal("memory leak: allocating without ResourceMark");
      }
    }
  }
}

#endif // ASSERT

// We're building a linked list of stack-allocated objects, with the current
// head recorded in the current Thread object and removed by the destructor.
// gcc doesn't recognize this and thinks we're doing something potentially
// problematic by recording a stack pointer in a non-stack location.  We
// should be able to disable the -Wdangling-pointer warning, but attempts to
// do so either here or Thread::set_current_resource_mark_state have failed.
// So we put this function out of line in the hope of obfuscating the
// connection between the stack value and the location where it is being
// stored.
ResourceMarkForThread::ResourceMarkForThread(Thread* thread)
  : _thread(thread),
    _state(thread->resource_area(), thread->current_resource_mark_state())
{
  assert(thread == Thread::current(), "not the current thread");
  thread->set_current_resource_mark_state(&_state);
}

ResourceArea* SafeResourceMark::_nothreads_resource_area = nullptr;
const ResourceMarkState* SafeResourceMark::_nothreads_current_state = nullptr;

Thread* SafeResourceMark::current_thread_or_null() {
  if (Threads::number_of_threads() == 0) {
    return nullptr;
  } else {
    return Thread::current();
  }
}

ResourceArea* SafeResourceMark::resource_area(Thread* thread) {
  if (thread != nullptr) {
    return thread->resource_area();
  } else {
    ResourceArea* ra = _nothreads_resource_area;
    if (ra == nullptr) {
      // Lazily create the early resource area.
      // Use a size which is not a standard since pools may not exist yet either.
      ra = new (mtInternal) ResourceArea(Chunk::non_pool_size);
      _nothreads_resource_area = ra;
    }
    return ra;
  }
}

const ResourceMarkState* SafeResourceMark::current_state(Thread* thread) {
  if (thread != nullptr) {
    return thread->current_resource_mark_state();
  } else {
    return _nothreads_current_state;
  }
}

void SafeResourceMark::set_current_state(Thread* thread,
                                         const ResourceMarkState* state) {
  if (thread != nullptr) {
    thread->set_current_resource_mark_state(state);
  } else {
    _nothreads_current_state = state;
  }
}

//------------------------------ResourceMark-----------------------------------
// The following routines are declared in allocation.hpp and used everywhere:

// Allocation in thread-local resource area
extern char* resource_allocate_bytes(size_t size, AllocFailType alloc_failmode) {
  return Thread::current()->resource_area()->allocate_bytes(size, alloc_failmode);
}
extern char* resource_allocate_bytes(Thread* thread, size_t size, AllocFailType alloc_failmode) {
  return thread->resource_area()->allocate_bytes(size, alloc_failmode);
}

extern char* resource_reallocate_bytes( char *old, size_t old_size, size_t new_size, AllocFailType alloc_failmode){
  return (char*)Thread::current()->resource_area()->Arealloc(old, old_size, new_size, alloc_failmode);
}

extern void resource_free_bytes( Thread* thread, char *old, size_t size ) {
  thread->resource_area()->Afree(old, size);
}
