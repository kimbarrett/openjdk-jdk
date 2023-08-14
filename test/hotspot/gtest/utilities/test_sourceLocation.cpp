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
#include "utilities/globalDefinitions.hpp"
#include "utilities/sourceLocation.hpp"
#include "unittest.hpp"

#if HAS_SOURCE_LOCATION_SUPPORT

struct TestSourceLocationInfo {
  SourceLocation _location;
  const char* _file;
  const char* _function;
  size_t _line;
};

static TestSourceLocationInfo get_source_location_info() {
  TestSourceLocationInfo info = {
    // __LINE__ must be on same line as current().
    SourceLocation::current(), __FILE__, __func__, __LINE__
  };

  return info;
}

TEST(TestSourceLocation, test_has_support) {
  TestSourceLocationInfo info = get_source_location_info();
  ASSERT_STREQ(info._location.file_name(), info._file);
  ASSERT_STREQ(info._location.function_name(), info._function);
  ASSERT_EQ(info._location.line(), info._line);
}

#else // !HAS_SOURCE_LOCATION_SUPPORT

TEST(TestSourceLocation, test_no_support) {
  SourceLocation loc = SourceLocation::current();
  ASSERT_STREQ(loc.file_name(), "unknown_file");
  ASSERT_STREQ(loc.function_name(), "");
  ASSERT_EQ(loc.line(), size_t(0));
}

#endif // HAS_SOURCE_LOCATION_SUPPORT
