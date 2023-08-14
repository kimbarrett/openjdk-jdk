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

#ifndef SHARE_UTILITIES_SOURCE_LOCATION_HPP
#define SHARE_UTILITIES_SOURCE_LOCATION_HPP

#include <stdlib.h>

// SourceLocation provides a stand-in for C++20 std::source_location, for use
// until we move to C++20.
//
// Most supported compilers have implemented std::source_location, and that
// implementation uses a set of compiler intrinsics that we can use directly
// without otherwise using C++20.  The intrinsics are __builtin_FILE(),
// __builtin_FUNCTION(), and __builtin_LINE().  These names are common to all
// of gcc, clang, and Visual Studio.
//
// We define the macro HAS_SOURCE_LOCATION_SUPPORT to 1 or 0, depending on
// whether the intrinsic support is available.  When the value is 0, only
// dummy information is provided by SourceLocation.

// Intrinsics were added in Visual Studio 2019 16.6.
// https://github.com/microsoft/STL/issues/54
// The minimum supported version of Visual Studio is past that, so we can rely
// on the intrinsics being available.
#if defined(TARGET_COMPILER_visCPP)
#define HAS_SOURCE_LOCATION_SUPPORT 1

// Intrinsics were added in clang 10.
#elif defined(__clang_major__)
#if __clang_major__ >= 10
#define HAS_SOURCE_LOCATION_SUPPORT 1
#endif

// Intrinsics were added in gcc4.8.  The minimum supported version of gcc is
// past that, so we can rely on the intrinsics being available.
#elif defined(__GNUC__)
#define HAS_SOURCE_LOCATION_SUPPORT 1

// Default to not having the intrinsics available.
#else
#define HAS_SOURCE_LOCATION_SUPPORT 0

#endif // End platform dispatch

class SourceLocation {
  struct Poison {};             // Prevent passing args ot current() & constructor

  const char* _file_name;
  const char* _function_name;
  size_t _line;

public:
  constexpr const char* file_name() const { return _file_name; }
  constexpr const char* function_name() const { return _function_name; }
  constexpr size_t line() const { return _line; }
  constexpr size_t column() const { return 0; }

#if HAS_SOURCE_LOCATION_SUPPORT
  constexpr SourceLocation(Poison = Poison(),
                           const char* file_name = __builtin_FILE(),
                           const char* function_name = __builtin_FUNCTION(),
                           size_t line = __builtin_LINE())
    : _file_name(file_name),
      _function_name(function_name),
      _line(line)
  {}

#else
  constexpr SourceLocation()
    : _file_name("unknown_file"),
      _function_name(""),
      _line(0)
  {}

#endif // HAS_SOURCE_LOCATION_SUPPORT

  static constexpr SourceLocation
  current(Poison = Poison(),
          const SourceLocation& location = SourceLocation()) {
    return location;
  }
};

#endif // SHARE_UTILITIES_SOURCE_LOCATION_HPP
