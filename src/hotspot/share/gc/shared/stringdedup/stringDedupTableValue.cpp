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

#include "precompiled.hpp"
#include "gc/shared/oopStorage.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/stringdedup/stringDedupTable.hpp"
#include "gc/shared/stringdedup/stringDedupTableValue.inline.hpp"
#include "utilities/debug.hpp"

StringDedup::TableValue::TableValue() : _p(nullptr) {}

StringDedup::TableValue::TableValue(oop* array_ref, bool latin1) :
  _p(reinterpret_cast<char*>(array_ref) + (latin1 ? Latin1Tag : NotLatin1Tag))
{
  assert(is_aligned(array_ref, TagAlignment), "alignment doesn't support encoding latin1");
}

void StringDedup::TableValue::release(OopStorage* storage) {
  assert(!is_empty(), "precondition");
  oop* p = storage_entry();
  NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(p, nullptr);
  storage->release(p);
}

#ifndef PRODUCT

void StringDedup::TableValue::print() const { print_on(tty); }

void StringDedup::TableValue::print_on(outputStream* st) const {
  if (is_empty()) {
    st->print("empty value");
  } else {
    st->print("TableValue: " PTR_FORMAT, p2i(peek()));
  }
}

#endif // !PRODUCT
