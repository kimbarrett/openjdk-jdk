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
#include "runtime/threadCrashProtection.hpp"

#include <setjmp.h>

// Access to a thread's protection object may occur within a signal handler,
// where use of THREAD_LOCAL is unsafe.  Hence we use a Thread member to hold
// the protection object, and use Thread::current_or_null_safe() to obtain the
// current thread, if there is one.

ThreadCrashProtection::ThreadCrashProtection(Thread* t, void* unwind_context) :
  _old_protection(t->crash_protection()),
  _unwind_context(unwind_context)
{
  assert(t != nullptr, "precondition");
  assert(t == Thread::current_or_null_safe(), "precondition");
}

ThreadCrashProtection::~ThreadCrashProtection() {
  Thread* t = Thread::current_or_null_safe();
  assert(t != nullptr, "invariant");
  t->set_crash_protection(_old_protection);
}

bool ThreadCrashProtection::is_protected() {
  Thread* t = Thread::current_or_null_safe();
  return (t != nullptr) && (t->crash_protection() != nullptr);
}

void ThreadCrashProtection::unwind_if_protected() {
  Thread* t = Thread::current_or_null_safe();
  if (t != nullptr) {
    ThreadCrashProtection* current = t->crash_protection();
    if (current != nullptr) {
      t->set_crash_protection(current->_old_protection);
      longjmp(*static_cast<jmp_buf*>(current->_unwind_context), 1);
    }
  }
}

bool ThreadCrashProtection::call_impl(Invoker invoke, void* callback) {
  Thread* t = Thread::current_or_null_safe();
  if (t != nullptr) {
    return call_with_protection(invoke, callback, t);
  } else {
    invoke(callback);
    return true;
  }
}

// call_with_protection() has platform-dependent implementation.

