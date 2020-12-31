/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLEVALUE_INLINE_HPP
#define SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLEVALUE_INLINE_HPP

#include "gc/shared/stringdedup/stringDedupTableValue.hpp"
#include "oops/access.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

inline oop* StringDedup::TableValue::storage_entry() const {
  return static_cast<oop*>(align_down(_p, TagAlignment));
}

template<DecoratorSet decorators>
inline typeArrayOop StringDedup::TableValue::access() const {
  assert(!is_empty(), "accessing uninitialized value");
  // Resolve Access expression template before casting.
  oop obj = NativeAccess<decorators>::oop_load(storage_entry());
  return (typeArrayOop)obj;
}

inline typeArrayOop StringDedup::TableValue::resolve() const {
  return access<ON_PHANTOM_OOP_REF>();
}

inline typeArrayOop StringDedup::TableValue::peek() const {
  return access<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>();
}

inline bool StringDedup::TableValue::is_empty() const {
  return _p == nullptr;
}

inline bool StringDedup::TableValue::is_latin1() const {
  static_assert(Latin1Tag == 0, "precondition");
  assert(!is_empty(), "precondition");
  return is_aligned(_p, TagAlignment);
}

#endif // SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLEVALUE_INLINE_HPP
