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


#ifndef SHARE_RUNTIME_THREADCRASHPROTECTION_HPP
#define SHARE_RUNTIME_THREADCRASHPROTECTION_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class Thread;

// Invoke a function in a context where "crashes" (assertion failures,
// hardware exceptions, &etc) in the current thread don't terminate the
// program. Instead of crashing, the call is aborted and the call stack is
// unwound to the protector, where execution resumes.  The unwinding is "best
// effort" and may leave the program in an inconsistent state.  Hence this
// facility shouldn't be used for most production purposes.  It is primarily
// intended for use in debugging contexts, where having an operation
// unintentionally terminate the program would be counterproductive.
//
// Note: The unwinding is implemented using setjmp/longjmp. Hence there is a
// risk of invoking undefined behavior, since (C++14 18.10/4) a setjmp/longjmp
// call pair has undefined behavior if replacing the setjmp and longjmp by
// catch and throw would invoke any non-trivial destructors for any automatic
// objects.  Typically what this means is that such destructors will not be
// executed, with consequences such as memory leaks or states not being
// updated or reverted.
class ThreadCrashProtection : public StackObj {
public:
  // Invokes callback() within a protected scope.  The callback must be a
  // nullary function or function object, and the result of the call is
  // ignored.  Returns true if the invocation completes normally, false if it
  // was aborted.  No protection is established if there is no current thread.
  template<typename F>
  static bool call(F callback) {
    Lookup<F> lookup{&callback};
    return call_impl(&lookup.invoke, lookup._callback);
  }

  // Returns true if the current thread is within a protected scope.
  // Returns false if there is no current thread.
  static bool is_protected();

  // If the current thread is within a protected scope, removes the protection
  // and aborts the callback's invocation, resuming execution in the
  // protector.  Does nothing if there is no current thread.
  static void unwind_if_protected();

private:
  // Prepare protected scope.
  // precondition: t is the current thread.
  ThreadCrashProtection(Thread* t, void* unwind_context);

  // End this protected scope.
  ~ThreadCrashProtection();

  template<typename F> struct Lookup;
  using Invoker = void (*)(void* callback);

  // Type-erased implementation of call().
  static bool call_impl(Invoker invoker, void* callback);

  // Invoke within a protected scope.
  // precondition: t is the current thread.
  static bool call_with_protection(Invoker invoker, void* callback, Thread* t);

  // This thread's previous protection state.
  ThreadCrashProtection* _old_protection;

  // Object used by unwind_if_protected() to perform the unwind.
  void* _unwind_context;
};

// Invocation of a function object.
template<typename F>
struct ThreadCrashProtection::Lookup {
  void* _callback;

  explicit Lookup(F* callback) : _callback(callback) {}

  static void invoke(void* callback) {
    (*static_cast<F*>(callback))();
  }
};

// Invocation of a function.
template<typename T>
struct ThreadCrashProtection::Lookup<T (**)()> {
  void* _callback;

  explicit Lookup(T (**callback)()) :
    _callback(CAST_FROM_FN_PTR(void*, *callback))
  {}

  static void invoke(void* callback) {
    (CAST_TO_FN_PTR(T (*)(), callback))();
  }
};

#endif // SHARE_RUNTIME_THREADCRASHPROTECTION_HPP
