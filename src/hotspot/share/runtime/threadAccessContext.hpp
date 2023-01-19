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


#ifndef SHARE_RUNTIME_THREADACCESSCONTEXT_HPP
#define SHARE_RUNTIME_THREADACCESSCONTEXT_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

class Thread;

// A thread may pause another thread to examine it.  While doing so, the
// examiner thread must take care to not perform certain operations, because
// the paused thread might be in a state where that would cause problems.
// Specifically, the examining thread must not
//
// (1) Make a potentially blocking attempt to lock a mutex.  The paused thread
// may be holding that mutex, resulting in deadlock.
//
// (2) Enter a nonrecursive ThreadCritical section.  The paused thread may be
// in ThreadCritical, resulting in deadlock.
//
// (3) Attempt to allocate memory.
//
// An object of this class establishes a context in which a call to
// assert_not_active() will fail an assertion and error.  Calls to that
// function are sprinkled about in the code to catch the described uses.
class ThreadAccessContext : public StackObj {
#ifdef ASSERT
  bool _old_state;
  ThreadAccessContext(Thread* t);
#endif // ASSERT

public:
  ThreadAccessContext() NOT_DEBUG(= default);
  ~ThreadAccessContext() NOT_DEBUG(= default);

  // Fail assertion if the current thread has a ThreadAccessContext.
  static void assert_not_active() NOT_DEBUG_RETURN;
};

#endif // SHARE_RUNTIME_THREADACCESSCONTEXT_HPP

