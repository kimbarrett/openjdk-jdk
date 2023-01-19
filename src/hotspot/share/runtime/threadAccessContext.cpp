/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "runtime/thread.hpp"
#include "runtime/threadAccessContext.hpp"
#include "utilities/debug.hpp"
#include "utilities/vmError.hpp"

#ifdef ASSERT

ThreadAccessContext::ThreadAccessContext(Thread* t) :
  _old_state(t->in_thread_access_context())
{
  t->set_in_thread_access_context(true);
}

ThreadAccessContext::ThreadAccessContext() :
  ThreadAccessContext(Thread::current())
{}

ThreadAccessContext::~ThreadAccessContext() {
  Thread::current()->set_in_thread_access_context(_old_state);
}

void ThreadAccessContext::assert_not_active() {
  Thread* t = Thread::current_or_null_safe();
  assert((t == nullptr) ||      // No current thread, so can't be active.
         !t->in_thread_access_context() ||
         // Don't complain if already in an error for this thread.
         // This could still run into problems the protection is supposed to be
         // checking for, but we're already in trouble.
         VMError::is_error_reported_in_current_thread(),
         "Thread access context is active");
}

#endif // ASSERT
