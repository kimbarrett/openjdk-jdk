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

#ifndef SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPCONFIG_HPP
#define SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPCONFIG_HPP

#include "gc/shared/stringdedup/stringDedup.hpp"
#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

// Provides access to canonicalized configuration parameter values.  There
// is only one instance of this class.  This object captures the various
// StringDeduplicationXXX command line option values, massages them, and
// provides error checking support.
class StringDedup::Config : public CHeapObj<mtGC> {
  int _initial_table_size;
  int _age_threshold;
  double _load_factor_for_growth;
  double _load_factor_for_shrink;
  double _load_factor_target;
  size_t _minimum_dead_for_cleanup;
  double _dead_factor_for_cleanup;
  uint64_t _hash_seed;

  Config();

public:
  static void initialize();
  static void destroy();

  // Perform ergonomic adjustments and error checking.
  // Returns true on success, false if some error check failed.
  static bool ergo_initialize();

  int initial_table_size() const;
  int age_threshold() const;
  uint64_t hash_seed() const;

  bool should_grow_table(size_t table_size, size_t entry_count) const;
  bool should_shrink_table(size_t table_size, size_t entry_count) const;
  int desired_table_size(size_t entry_count) const;
  bool should_cleanup_table(size_t entry_count, size_t dead_count) const;
};

#endif // SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPCONFIG_HPP

