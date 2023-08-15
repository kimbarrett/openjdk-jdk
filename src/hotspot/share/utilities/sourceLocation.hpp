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

// SourceLocation provides a stand-in for C++20 std::source_location, for use
// until we move to C++20.
//
// All supported compilers have implemented std::source_location, and that
// implementation uses a set of compiler intrinsics that we can use directly
// without otherwise using C++20.  The intrinsics are __builtin_FILE(),
// __builtin_FUNCTION(), and __builtin_LINE().  These names are common to all
// of gcc, clang, and Visual Studio.  Visual Studio and clang also provide
// __builtin_COLUMN(), but gcc does not.  We don't provide column information
// in this stand-in.

class SourceLocation {
  struct Poison {};

  const char* _file_name;
  const char* _function_name;
  unsigned _line;

  constexpr SourceLocation(const char* file_name,
                           const char* function_name,
                           unsigned line)
    : _file_name(file_name),
      _function_name(function_name),
      _line(line)
  {}

public:
  constexpr const char* file_name() const { return _file_name; }

  constexpr const char* function_name() const { return _function_name; }

  // Line numbers are 1-indexed, with 0 indicating unknown - C++20 Table 38.
  constexpr unsigned line() const { return _line; }

  // Column numbers are 1-indexed, with 0 indicating unknown - C++20 Table 38.
  // Column information is not supported by this implementation because gcc
  // doesn't provide the associated intrinsic and we don't really need it.
  constexpr unsigned column() const { return 0; }

  constexpr SourceLocation() : SourceLocation("", "", 0) {}

  static constexpr SourceLocation
  current(Poison = Poison(),    // Prevent passing arguments.
          const char* file_name = __builtin_FILE(),
          const char* function_name = __builtin_FUNCTION(),
          // gcc's __builtin_LINE returns int.
          unsigned line = static_cast<unsigned>(__builtin_LINE())) {
    return SourceLocation(file_name, function_name, line);
  }
};

#endif // SHARE_UTILITIES_SOURCE_LOCATION_HPP
