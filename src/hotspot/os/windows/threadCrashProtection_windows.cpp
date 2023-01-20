/*
 * Copyright (c) 2017, 2022, Oracle and/or its affiliates. All rights reserved.
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

// Protects the callback call so that both raised OS exceptions and VM errors
// cause a jump back into this function to return false. If neither occurs then
// returns true.
bool ThreadCrashProtection::call_with_protection(Invoker invoker,
                                                 void* callback,
                                                 Thread* t) {
  jmp_buf jmpbuf;
  ThreadCrashProtection protection{t, &jmpbuf};
  __try {
    if (setjmp(jmpbuf) == 0) {
      t->set_crash_protection(&protection); // Install now that jmpbuf initialized.
      invoker(callback);
      // Success.  Protection will be removed by destructor.
      return true;
    } else {
      // Exited call() via longjmp.  Protection was removed before unwind.
      return false;
    }
  } __except(EXCEPTION_EXECUTE_HANDLER) {
    // Exited call() via structured exception handling.
    // Protection will be removed by destructor.
    return false;
  }
}
