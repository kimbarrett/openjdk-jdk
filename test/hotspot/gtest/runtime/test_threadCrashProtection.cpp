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
 */

#include "precompiled.hpp"
#include "runtime/threadCrashProtection.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

class TestThreadCrashProtectionCallback {
  bool _was_protected;

protected:
  TestThreadCrashProtectionCallback() : _was_protected(false) {}
  ~TestThreadCrashProtectionCallback() = default;

  void record_was_protected() {
    _was_protected = ThreadCrashProtection::is_protected();
  }

public:
  bool was_protected() const { return _was_protected; }
};

TEST_VM(TestThreadCrashProtection, normal) {
  class NormalCall : public TestThreadCrashProtectionCallback {
  public:
    void call() {
      record_was_protected();
    }
  } normal{};

  ASSERT_TRUE(ThreadCrashProtection::call([&] { normal.call(); }));
  ASSERT_TRUE(normal.was_protected());
}

TEST_VM(TestThreadCrashProtection, crash) {
  class CrashingCall : public TestThreadCrashProtectionCallback {
  public:
    void call() {
      record_was_protected();
      fatal("crashing for test");
    }
  } crasher{};

  ASSERT_FALSE(ThreadCrashProtection::call([&] { crasher.call(); }));
  ASSERT_TRUE(crasher.was_protected());
}

