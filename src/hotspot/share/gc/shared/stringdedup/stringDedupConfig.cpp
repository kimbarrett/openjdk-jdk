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
#include "classfile/altHashing.hpp"
#include "gc/shared/stringdedup/stringDedupConfig.hpp"
#include "logging/log.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

int StringDedup::Config::initial_table_size() const {
  return _initial_table_size;
}

int StringDedup::Config::age_threshold() const {
  return _age_threshold;
}

bool StringDedup::Config::should_cleanup_table(size_t entry_count,
                                               size_t dead_count) const {
  return (dead_count > _minimum_dead_for_cleanup) &&
         (dead_count > (entry_count * _dead_factor_for_cleanup));
}

uint64_t StringDedup::Config::hash_seed() const {
  return _hash_seed;
}

static uint64_t initial_hash_seed() {
  if (StringDeduplicationHashSeed != 0) {
    return StringDeduplicationHashSeed;
  } else {
    return AltHashing::compute_seed();
  }
}

// Primes after 1000 * 2^N and 1000 * (2^N + 2^(N-1)) for integer N.
constexpr size_t good_sizes[] = {
  1009, 1511, 2003, 3001, 4001, 6007, 8009, 12007, 16001, 24001, 32003, 48017,
  64007, 96001, 128021, 192007, 256019, 384001, 512009, 768013, 1024021,
  1536011, 2048003, 3072001, 4096013, 6144001, 8192003, 12288011, 16384001,
  24576001, 32768011, 49152001, 65536043, 98304053, 131072003, 196608007,
  262144009, 393216007, 524288057, 786432001, 1048576019, 1572864001 };

static int good_size(size_t n) {
  size_t result = good_sizes[ARRAY_SIZE(good_sizes) - 1];
  for (size_t i = 0; i < ARRAY_SIZE(good_sizes); ++i) {
    if (n <= good_sizes[i]) {
      result = good_sizes[i];
      break;
    }
  }
  return checked_cast<int>(result);
}

bool StringDedup::Config::should_grow_table(size_t table_size,
                                            size_t entry_count) const {
  return (entry_count / _load_factor_for_growth) > table_size;
}

bool StringDedup::Config::should_shrink_table(size_t table_size,
                                              size_t entry_count) const {
  return (entry_count / _load_factor_for_shrink) < table_size;
}

int StringDedup::Config::desired_table_size(size_t entry_count) const {
  return good_size(entry_count / _load_factor_target);
}

bool StringDedup::Config::ergo_initialize() {
  if (!UseStringDeduplication) {
    return true;
  } else if (!UseG1GC) {
    // String deduplication requested but not supported by the selected GC.
    // Warn and force disable, but don't error.
    assert(!FLAG_IS_DEFAULT(UseStringDeduplication),
           "Enabled by default for GC that doesn't support it");
    log_warning(gc, stringdedup)("String Deduplication disabled: "
                                 "not supported by selected GC");
    FLAG_SET_ERGO(UseStringDeduplication, false);
    return true;
  }

  // UseStringDeduplication is enabled.  Check parameters.
  bool result = true;

  // ShrinkTableLoad <= TargetTableLoad <= GrowTableLoad.
  if (StringDeduplicationShrinkTableLoad > StringDeduplicationTargetTableLoad) {
    JVMFlag::printError(true,
                        "StringDeduplicationShrinkTableLoad (%f) must not exceed "
                        "StringDeduplicationTargetTableLoad (%f)",
                        StringDeduplicationShrinkTableLoad,
                        StringDeduplicationTargetTableLoad);
    result = false;
  }
  if (StringDeduplicationTargetTableLoad > StringDeduplicationGrowTableLoad) {
    JVMFlag::printError(true,
                        "StringDeduplicationTargetTableLoad (%f) must not exceed "
                        "StringDeduplicationGrowTableLoad (%f)",
                        StringDeduplicationTargetTableLoad,
                        StringDeduplicationGrowTableLoad);
    result = false;
  }

  return result;
}

StringDedup::Config::Config() :
  _initial_table_size(good_size(StringDeduplicationInitialTableSize)),
  _age_threshold(StringDeduplicationAgeThreshold),
  _load_factor_for_growth(StringDeduplicationGrowTableLoad),
  _load_factor_for_shrink(StringDeduplicationShrinkTableLoad),
  _load_factor_target(StringDeduplicationTargetTableLoad),
  _minimum_dead_for_cleanup(StringDeduplicationCleanupDeadMinimum),
  _dead_factor_for_cleanup(percent_of(StringDeduplicationCleanupDeadPercent, 100)),
  _hash_seed(initial_hash_seed())
{}

void StringDedup::Config::initialize() {
  assert(_config == nullptr, "already initialized");
  _config = new Config();
}

void StringDedup::Config::destroy() {
  delete _config;
  _config = nullptr;
}
