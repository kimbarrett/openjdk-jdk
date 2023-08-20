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
#include "utilities/checkedCast.hpp"
#include "utilities/globalDefinitions.hpp"
#include <limits>
#include "unittest.hpp"

#ifdef __GNUC__
#pragma gcc diagnostic warning "-Wconversion"
#endif

using Cast = CheckedCastImpl;

template<typename T>
struct TestCheckedCastValues {
  static TestCheckedCastValues values;

  T minus_one = static_cast<T>(-1);
  T zero = static_cast<T>(0);
  T one = static_cast<T>(1);
  T min = std::numeric_limits<T>::lowest();
  T max = std::numeric_limits<T>::max();
};

template<typename T>
TestCheckedCastValues<T> TestCheckedCastValues<T>::values{};

template<typename T>
struct TestCheckedCastFloatValues : public TestCheckedCastValues<T> {
  static TestCheckedCastFloatValues values;

  T minus_one_point_five = static_cast<T>(-1.5);
  T one_point_five = static_cast<T>(1.5);
};

template<typename T>
TestCheckedCastFloatValues<T> TestCheckedCastFloatValues<T>::values{};

template<typename Small, typename Large>
struct TestCheckedCastSmallAsLargeValues {
  static TestCheckedCastSmallAsLargeValues values;

  Large min = static_cast<Large>(std::numeric_limits<Small>::lowest());
  Large max = static_cast<Large>(std::numeric_limits<Small>::max());
};

template<typename Small, typename Large>
TestCheckedCastSmallAsLargeValues<Small, Large>
TestCheckedCastSmallAsLargeValues<Small, Large>::values{};

//////////////////////////////////////////////////////////////////////////////
// Checked casts between integral types of different sizes.
// Test narrowing to verify checking.
// Test widening to verify no compiler warnings for tautological comparisons.

template<typename Small, typename Large>
struct TestCheckedCastIntegerValues {
  static TestCheckedCastIntegerValues values;

  TestCheckedCastValues<Small> small;
  TestCheckedCastValues<Large> large;
  TestCheckedCastSmallAsLargeValues<Small, Large> small_as_large;
};

template<typename Small, typename Large>
TestCheckedCastIntegerValues<Small, Large>
TestCheckedCastIntegerValues<Small, Large>::values{};

TEST(TestCheckedCast, signed_integers) {
  using T32 = int32_t;
  using T64 = int64_t;
  using Values = TestCheckedCastIntegerValues<T32, T64>;
  const Values& values = Values::values;

  EXPECT_TRUE(Cast::check<T32>(values.large.minus_one));
  EXPECT_TRUE(Cast::check<T32>(values.large.zero));
  EXPECT_TRUE(Cast::check<T32>(values.large.one));
  EXPECT_FALSE(Cast::check<T32>(values.large.min));
  EXPECT_FALSE(Cast::check<T32>(values.large.max));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.max));

  EXPECT_TRUE(Cast::check<T64>(values.small.minus_one));
  EXPECT_TRUE(Cast::check<T64>(values.small.zero));
  EXPECT_TRUE(Cast::check<T64>(values.small.one));
  EXPECT_TRUE(Cast::check<T64>(values.small.min));
  EXPECT_TRUE(Cast::check<T64>(values.small.max));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.max));
}

TEST(TestCheckedCast, unsigned_integers) {
  using T32 = uint32_t;
  using T64 = uint64_t;
  using Values = TestCheckedCastIntegerValues<T32, T64>;
  const Values& values = Values::values;

  EXPECT_FALSE(Cast::check<T32>(values.large.minus_one));
  EXPECT_TRUE(Cast::check<T32>(values.large.zero));
  EXPECT_TRUE(Cast::check<T32>(values.large.one));
  EXPECT_TRUE(Cast::check<T32>(values.large.min));
  EXPECT_FALSE(Cast::check<T32>(values.large.max));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.min));

  EXPECT_TRUE(Cast::check<T64>(values.small.minus_one));
  EXPECT_TRUE(Cast::check<T64>(values.small.zero));
  EXPECT_TRUE(Cast::check<T64>(values.small.one));
  EXPECT_TRUE(Cast::check<T64>(values.small.min));
  EXPECT_TRUE(Cast::check<T64>(values.small.max));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.max));
}

TEST(TestCheckedCast, unsigned_to_signed_integers) {
  using T32 = int32_t;
  using T64 = uint64_t;
  using Values = TestCheckedCastIntegerValues<T32, T64>;
  const Values& values = Values::values;

  EXPECT_FALSE(Cast::check<T32>(values.large.minus_one));
  EXPECT_TRUE(Cast::check<T32>(values.large.zero));
  EXPECT_TRUE(Cast::check<T32>(values.large.one));
  EXPECT_TRUE(Cast::check<T32>(values.large.min));
  EXPECT_FALSE(Cast::check<T32>(values.large.max));
  EXPECT_FALSE(Cast::check<T32>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.max));
}

TEST(TestCheckedCast, signed_to_unsigned_integers) {
  using T32 = uint32_t;
  using T64 = int64_t;
  using Values = TestCheckedCastIntegerValues<T32, T64>;
  const Values& values = Values::values;

  EXPECT_FALSE(Cast::check<T32>(values.large.minus_one));
  EXPECT_TRUE(Cast::check<T32>(values.large.zero));
  EXPECT_TRUE(Cast::check<T32>(values.large.one));
  EXPECT_FALSE(Cast::check<T32>(values.large.min));
  EXPECT_FALSE(Cast::check<T32>(values.large.max));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T32>(values.small_as_large.max));
}

TEST(TestCheckedCast, unsigned_to_wide_signed_integers) {
  using T32 = uint32_t;
  using T64 = int64_t;
  using Values = TestCheckedCastIntegerValues<T32, T64>;
  const Values& values = Values::values;

  EXPECT_TRUE(Cast::check<T64>(values.small.minus_one));
  EXPECT_TRUE(Cast::check<T64>(values.small.zero));
  EXPECT_TRUE(Cast::check<T64>(values.small.one));
  EXPECT_TRUE(Cast::check<T64>(values.small.min));
  EXPECT_TRUE(Cast::check<T64>(values.small.max));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.max));
}

TEST(TestCheckedCast, signed_to_wide_unsigned_integers) {
  using T32 = int32_t;
  using T64 = uint64_t;
  using Values = TestCheckedCastIntegerValues<T32, T64>;
  const Values& values = Values::values;

  EXPECT_FALSE(Cast::check<T64>(values.small.minus_one));
  EXPECT_TRUE(Cast::check<T64>(values.small.zero));
  EXPECT_TRUE(Cast::check<T64>(values.small.one));
  EXPECT_FALSE(Cast::check<T64>(values.small.min));
  EXPECT_TRUE(Cast::check<T64>(values.small.max));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.min));
  EXPECT_TRUE(Cast::check<T64>(values.small_as_large.max));
}

//////////////////////////////////////////////////////////////////////////////
// Checked casts between floating point types of different sizes.
// Test narrowing to verify checking.
// Test widening to verify no compiler warnings for tautological comparisons.

TEST(TestCheckedCast, floating_point) {
  using Small = float;
  using Large = double;
  using SmallValues = TestCheckedCastFloatValues<Small>;
  using LargeValues = TestCheckedCastFloatValues<Large>;
  using SmallAsLargeValues = TestCheckedCastSmallAsLargeValues<Small, Large>;
  const SmallValues& small = SmallValues::values;
  const LargeValues& large = LargeValues::values;
  const SmallAsLargeValues& small_as_large = SmallAsLargeValues::values;

  EXPECT_TRUE(Cast::check<Small>(large.minus_one));
  EXPECT_TRUE(Cast::check<Small>(large.zero));
  EXPECT_TRUE(Cast::check<Small>(large.one));
  EXPECT_TRUE(Cast::check<Small>(large.minus_one_point_five));
  EXPECT_TRUE(Cast::check<Small>(large.one_point_five));
  EXPECT_FALSE(Cast::check<Small>(large.min));
  EXPECT_FALSE(Cast::check<Small>(large.max));
  EXPECT_TRUE(Cast::check<Small>(small_as_large.min));
  EXPECT_TRUE(Cast::check<Small>(small_as_large.max));

  EXPECT_TRUE(Cast::check<Large>(small.minus_one));
  EXPECT_TRUE(Cast::check<Large>(small.zero));
  EXPECT_TRUE(Cast::check<Large>(small.one));
  EXPECT_TRUE(Cast::check<Large>(small.minus_one_point_five));
  EXPECT_TRUE(Cast::check<Large>(small.one_point_five));
  EXPECT_TRUE(Cast::check<Large>(small.min));
  EXPECT_TRUE(Cast::check<Large>(small.max));
  EXPECT_TRUE(Cast::check<Large>(small_as_large.min));
  EXPECT_TRUE(Cast::check<Large>(small_as_large.max));
}

//////////////////////////////////////////////////////////////////////////////
// Checked casts from floating point to integral.

TEST(TestCheckedCast, floating_point_to_int) {
  using Values = TestCheckedCastFloatValues<double>;
  const Values& values = Values::values;

  EXPECT_TRUE(Cast::check<int>(values.minus_one));
  EXPECT_TRUE(Cast::check<int>(values.zero));
  EXPECT_TRUE(Cast::check<int>(values.one));
  EXPECT_TRUE(Cast::check<int>(values.minus_one_point_five));
  EXPECT_TRUE(Cast::check<int>(values.one_point_five));
  EXPECT_FALSE(Cast::check<int>(values.min));
  EXPECT_FALSE(Cast::check<int>(values.max));

  using SmallAsLargeValues = TestCheckedCastSmallAsLargeValues<int, double>;
  const SmallAsLargeValues& small_as_large = SmallAsLargeValues::values;

  EXPECT_TRUE(Cast::check<int>(small_as_large.min));
  EXPECT_TRUE(Cast::check<int>(small_as_large.max));
}

TEST(TestCheckedCast, floating_point_to_unsigned) {
  using Values = TestCheckedCastFloatValues<double>;
  const Values& values = Values::values;

  EXPECT_FALSE(Cast::check<unsigned>(values.minus_one));
  EXPECT_TRUE(Cast::check<unsigned>(values.zero));
  EXPECT_TRUE(Cast::check<unsigned>(values.one));
  EXPECT_FALSE(Cast::check<unsigned>(values.minus_one_point_five));
  EXPECT_TRUE(Cast::check<unsigned>(values.one_point_five));
  EXPECT_FALSE(Cast::check<unsigned>(values.min));
  EXPECT_FALSE(Cast::check<unsigned>(values.max));

  using SmallAsLargeValues = TestCheckedCastSmallAsLargeValues<unsigned, double>;
  const SmallAsLargeValues& small_as_large = SmallAsLargeValues::values;

  EXPECT_TRUE(Cast::check<unsigned>(small_as_large.min));
  EXPECT_TRUE(Cast::check<unsigned>(small_as_large.max));
}

//////////////////////////////////////////////////////////////////////////////
// Checked casts from integral to floating point.

TEST(TestCheckedCast, int64_to_float) {
  using Values = TestCheckedCastValues<int64_t>;
  const Values& values = Values::values;

  EXPECT_TRUE(Cast::check<float>(values.minus_one));
  EXPECT_TRUE(Cast::check<float>(values.zero));
  EXPECT_TRUE(Cast::check<float>(values.one));
  EXPECT_TRUE(Cast::check<float>(values.min));
  EXPECT_TRUE(Cast::check<float>(values.max));
}

TEST(TestCheckedCast, uint64_to_float) {
  using Values = TestCheckedCastValues<uint64_t>;
  const Values& values = Values::values;

  EXPECT_TRUE(Cast::check<float>(values.minus_one));
  EXPECT_TRUE(Cast::check<float>(values.zero));
  EXPECT_TRUE(Cast::check<float>(values.one));
  EXPECT_TRUE(Cast::check<float>(values.min));
  EXPECT_TRUE(Cast::check<float>(values.max));
}

//////////////////////////////////////////////////////////////////////////////
// Checked casts from enum to integral.

TEST(TestCheckedCast, enums) {
  using I = int;
  enum class TestEnum : I {};
  using Values = TestCheckedCastValues<I>;
  const Values& values = Values::values;

  EXPECT_TRUE(Cast::check<I>(static_cast<TestEnum>(values.minus_one)));
  EXPECT_TRUE(Cast::check<I>(static_cast<TestEnum>(values.zero)));
  EXPECT_TRUE(Cast::check<I>(static_cast<TestEnum>(values.one)));
  EXPECT_TRUE(Cast::check<I>(static_cast<TestEnum>(values.min)));
  EXPECT_TRUE(Cast::check<I>(static_cast<TestEnum>(values.max)));
}
