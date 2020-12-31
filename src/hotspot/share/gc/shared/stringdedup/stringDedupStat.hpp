/*
 * Copyright (c) 2014, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPSTAT_HPP
#define SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPSTAT_HPP

#include "gc/shared/stringdedup/stringDedup.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ticks.hpp"

//
// Statistics gathered by the deduplication thread.
//
class StringDedup::Stat {
public:
  // Only phases that can be blocked, so not idle.
  enum class Phase {
    process,
    rebuild_table,
    cleanup_table
  };

private:
  // Counters
  uintx  _inspected;
  uintx  _skipped;
  uintx  _known;
  uintx  _new;
  uintx  _new_bytes;
  uintx  _deduped;
  uintx  _deduped_bytes;
  uintx  _deleted;

  // Phase counters for deduplication thread
  uintx  _concurrent;
  uintx  _idle;
  uintx  _process;
  uintx  _rebuild_table;
  uintx  _cleanup_table;
  uintx  _block;

  // Time spent by the deduplication thread in different phases
  Ticks _concurrent_start;
  Tickspan _concurrent_elapsed;
  Ticks _phase_start;
  Tickspan _idle_elapsed;
  Tickspan _process_elapsed;
  Tickspan _rebuild_table_elapsed;
  Tickspan _cleanup_table_elapsed;
  Tickspan _block_elapsed;

  void report_phase_start(const char* phase);
  void report_phase_end(const char* phase, Tickspan* elapsed);
  Tickspan* elapsed_for_phase(Phase phase);

  void log_times(const char* prefix) const;

public:
  Stat();

  void inc_inspected() {
    _inspected++;
  }

  void inc_skipped() {
    _skipped++;
  }

  void inc_known() {
    _known++;
  }

  void inc_new(uintx bytes) {
    _new++;
    _new_bytes += bytes;
  }

  void inc_deduped(uintx bytes) {
    _deduped++;
    _deduped_bytes += bytes;
  }

  void inc_deleted() {
    _deleted++;
  }

  void report_idle_start();
  void report_idle_end();

  void report_process_start();
  void report_process_end();

  void report_rebuild_table_start(int new_table_size,
                                  int old_table_size,
                                  size_t entry_count,
                                  size_t dead_count);
  void report_rebuild_table_end();

  void report_cleanup_table_start(size_t entry_count, size_t dead_count);
  void report_cleanup_table_end();

  void report_concurrent_start();
  void report_concurrent_end();

  void block_phase(Phase phase);
  void unblock_phase();

  void add(const Stat* const stat);
  void log_statistics(bool total) const;

  static void log_summary(const Stat* last_stat, const Stat* total_stat);
};

#endif // SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPSTAT_HPP
