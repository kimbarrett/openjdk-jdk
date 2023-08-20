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

#ifndef SHARE_UTILITIES_CHECKEDCAST_HPP
#define SHARE_UTILITIES_CHECKEDCAST_HPP

#include "metaprogramming/enableIf.hpp"
#include "utilities/debug.hpp"
#include <limits>
#include <type_traits>

// Convert a value to another type via static_cast, after checking that the
// the value is within the range for the destination type.
//
// Because of details of C++ conversion rules, including undefined and
// implementation-defined behavior in some cases, along with the possibility
// of tautological comparison warnings from some compilers, the implementation
// is not as simple as doing the obvious range checks.
//
// - Conversions between integral types verify the value is representable in
// the destination type.
//
// - Conversions between floating point types or between integral and floating
// point types verifies the value is representable in the destination type,
// possibly with some loss of precision.
//
// - If the value is an enum type and the destination type is an integral type
// then the verification uses the underlying type of the enum type.
//
// Note that some enum -> integral conversions are for compatibility with
// old code. Class scoped enums were used to work around ancient compilers
// that didn't implement class scoped static integral constants, and HotSpot
// code still has many examples of this. For others it might be sufficient to
// provide an explicit underlying type and either permit implicit conversions
// or use PrimitiveConversions::cast.
template<typename To, typename From>
constexpr To checked_cast(From from);

//////////////////////////////////////////////////////////////////////////////

class CheckedCastImpl {
  template<typename To, typename From,
           bool to_signed = std::is_signed<To>::value,
           bool from_signed = std::is_signed<From>::value,
           typename Enable = void>
  struct IntegerChecker;

  template<typename To, typename From, typename Enable = void>
  struct FloatChecker;

  template<typename To, typename From>
  struct IntegerToFloatChecker;

  template<typename To, typename From>
  struct FloatToIntegerChecker;

public:
  // Public check for unit test access.  Else private and befriend checked_cast.

  template<typename To, typename From,
           ENABLE_IF(std::is_integral<To>::value),
           ENABLE_IF(std::is_integral<From>::value)>
  static constexpr bool check(From from) {
    return IntegerChecker<To, From>()(from);
  }

  template<typename To, typename From,
           ENABLE_IF(std::is_floating_point<To>::value),
           ENABLE_IF(std::is_floating_point<From>::value)>
  static constexpr bool check(From from) {
    return FloatChecker<To, From>()(from);
  }

  template<typename To, typename From,
           ENABLE_IF(std::is_integral<To>::value),
           ENABLE_IF(std::is_floating_point<From>::value)>
  static constexpr bool check(From from) {
    return FloatToIntegerChecker<To, From>()(from);
  }

  template<typename To, typename From,
           ENABLE_IF(std::is_floating_point<To>::value),
           ENABLE_IF(std::is_integral<From>::value)>
  static constexpr bool check(From from) {
    return IntegerToFloatChecker<To, From>()(from);
  }

  template<typename To, typename From,
           ENABLE_IF(std::is_integral<To>::value),
           ENABLE_IF(std::is_enum<From>::value)>
  static constexpr bool check(From from) {
    return check<To>(static_cast<std::underlying_type_t<From>>(from));
  }
};

//////////////////////////////////////////////////////////////////////////////
// IntegerChecker<To, From>
// There are a lot of assumptions about integer representation here.
// C++20 requires two's-complement.

// If both types are signed or both are unsigned, only a narrowing conversion
// can lose information.  We check for such loss via a round-trip conversion.
// C++14 4.7 defines the result for unsigned types, and implementation-defined
// for signed types.  All supported implementations do the "obvious" discard
// of high-order bits when narrowing signed values.
template<typename To, typename From, bool to_signed, bool from_signed>
struct CheckedCastImpl::IntegerChecker<
  To, From, to_signed, from_signed,
  std::enable_if_t<to_signed == from_signed>>
{
  constexpr bool operator()(From from) const {
    return ((sizeof(To) >= sizeof(From)) ||
            (static_cast<From>(static_cast<To>(from)) == from));
  }
};

// Conversion from unsigned to signed is okay if value <= To's max.

template<typename To, typename From>
struct CheckedCastImpl::IntegerChecker<
  To, From, true /* to_signed */, false /* from_signed */,
  std::enable_if_t<(sizeof(From) >= sizeof(To))>>
{
  constexpr bool operator()(From from) const {
    auto to_smax = std::numeric_limits<To>::max();
    auto to_umax = static_cast<From>(to_smax);
    return from <= to_umax;
  }
};

// Avoid tautological comparison when sizeof(From) < sizeof(To).
template<typename To, typename From>
struct CheckedCastImpl::IntegerChecker<
  To, From, true /* to_signed */, false /* from_signed */,
  std::enable_if_t<(sizeof(From) < sizeof(To))>>
{
  constexpr bool operator()(From from) const {
    return true;
  }
};

// Conversion from signed to unsigned is okay when 0 <= value <= To's max.

template<typename To, typename From>
struct CheckedCastImpl::IntegerChecker<
  To, From, false /* to_signed */, true /* from_signed */,
  std::enable_if_t<(sizeof(From) > sizeof(To))>>
{
  constexpr bool operator()(From from) const {
    auto to_umax = std::numeric_limits<To>::max();
    auto to_smax = static_cast<From>(to_umax);
    return (from >= 0) && (from <= to_smax);
  }
};

// Avoid tautological comparison when sizeof(From) <= sizeof(To).
template<typename To, typename From>
struct CheckedCastImpl::IntegerChecker<
  To, From, false /* to_signed */, true /* from_signed */,
  std::enable_if_t<(sizeof(From) <= sizeof(To))>>
{
  constexpr bool operator()(From from) const {
    return from >= 0;
  }
};

//////////////////////////////////////////////////////////////////////////////
// FloatChecker<To, From>

// C++14 4.8 says floating point conversions are undefined behavior if the
// value is out of range for the destination type.

template<typename To, typename From, typename Enable>
struct CheckedCastImpl::FloatChecker {
  constexpr bool operator()(From from) const {
    // Implicit conversions from narrow To to wide From are okay.
    if (from < 0) {
      return from >= std::numeric_limits<To>::lowest();
    } else {
      return from <= std::numeric_limits<To>::max();
    }
  }
};

// Avoid tautological comparison when sizeof(To) >= sizeof(From).
template<typename To, typename From>
struct CheckedCastImpl::FloatChecker<
  To, From, std::enable_if_t<(sizeof(To) >= sizeof(From))>>
{
  constexpr bool operator()(From from) const {
    return true;
  }
};

//////////////////////////////////////////////////////////////////////////////
// IntegerToFloatChecker<To, From>

// C++14 4.9 says integral to floating point conversions are undefined
// behavior if the value is outside the range of the destination type.  Loss
// of precision may occur if the integral value cannot be represented exactly
// in the destination type.

template<typename To, typename From>
struct CheckedCastImpl::IntegerToFloatChecker {
  // Verify simplifying assumptions, to avoid complex code we can't test.
  static_assert(std::numeric_limits<To>::radix == 2, "assumption");
  static_assert(std::numeric_limits<From>::radix == 2, "assumption");
  // Range of integer From is enclosed by range of floating point To.
  static_assert(std::numeric_limits<From>::digits <=
                std::numeric_limits<To>::max_exponent,
                "assumption");

  constexpr bool operator()(From from) const {
    return true;
  }
};

//////////////////////////////////////////////////////////////////////////////
// FloatToIntegerChecker<To, From>

// C++14 4.9 says floating point conversions to integral types truncates, and
// the behavior is undefined if the truncated value can't be represented in
// the destination type.

template<typename To, typename From>
struct CheckedCastImpl::FloatToIntegerChecker {
  // Verify simplifying assumptions, to avoid complex code we can't test.
  static_assert(std::numeric_limits<To>::radix == 2, "assumption");
  static_assert(std::numeric_limits<From>::radix == 2, "assumption");
  // Range of integer To is enclosed by range of floating point From.
  static_assert(std::numeric_limits<To>::digits <=
                std::numeric_limits<From>::max_exponent,
                "assumption");

  // We're arguably a little more restrictive here than necessary.  If
  // floating point from is outside the integral range by only a fraction, the
  // specified truncation would bring it in range.  We could test for that by
  // using scalbn involving To's digits to produce a bound.  But that does
  // permit narrowing a value that really is outside the integral range
  // (before truncation).  Also, scalbn isn't (yet) constexpr.

  constexpr bool operator()(From from) const {
    if (from < 0) {
      // This is sufficient regardless of whether To is signed or unsigned.
      return from >= static_cast<From>(std::numeric_limits<To>::min());
    } else {
      return from <= static_cast<From>(std::numeric_limits<To>::max());
    }
  }
};

//////////////////////////////////////////////////////////////////////////////

template<typename To, typename From>
constexpr To checked_cast(From from) {
  assert(CheckedCastImpl::check<To>(from), "checked_cast failed");
  // The conversion must follow the checks.  Some checks test for cases where
  // the conversion would be undefined behavior.
  return static_cast<To>(from);
}

#endif // SHARE_UTILITIES_CHECKEDCAST_HPP
