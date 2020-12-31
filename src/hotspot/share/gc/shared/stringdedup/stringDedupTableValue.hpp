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

#ifndef SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLEVALUE_HPP
#define SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLEVALUE_HPP

#include "gc/shared/stringdedup/stringDedup.hpp"
#include "oops/accessDecorators.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/typeArrayOop.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

class OopStorage;
class outputStream;

// The value type for StringDedup::Table entries.  This class is a thin
// wrapper over an encoded pointer value.  The value is a weak OopStorage
// entry (an oop*) referring to a jbyte[] or jchar[], with the distinction
// encoded in the low bit of the storage pointer.
//
// 0 - the object is a jbyte[] using Latin1 encoding.
// 1 - the object is a jchar[] not using Latin1 encoding.
//
// A simpler representation would use WeakHandle storage entry and bool
// latin1 members, but that would increase the size of a Table entry from 3
// pointer-sized values to 4.  Since there may be a lot of entries, that
// size difference matters, and it's worth the effort to use a more compact
// representation.

class StringDedup::TableValue {
  void* _p;

  static const uintptr_t Latin1Tag = 0;
  static const uintptr_t NotLatin1Tag = 1;
  static const uintptr_t TagSize = 1;
  static const uintptr_t TagAlignment = 1 << TagSize;

  template<DecoratorSet decorators>
  typeArrayOop access() const;

public:
  TableValue();
  TableValue(oop* array_ref, bool latin1);

  oop* storage_entry() const;

  typeArrayOop resolve() const;
  typeArrayOop peek() const;
  void release(OopStorage* storage);
  bool is_empty() const;
  bool is_latin1() const;
  void print() const PRODUCT_RETURN;
  void print_on(outputStream* st) const PRODUCT_RETURN;
};

#endif // SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLEVALUE_HPP
