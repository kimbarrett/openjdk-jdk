/*
 * Copyright (c) 2014, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/stringdedup/stringDedupStat.hpp"
#include "logging/log.hpp"
#include "utilities/globalDefinitions.hpp"

// Support for log output formating
#define STRDEDUP_OBJECTS_FORMAT         UINTX_FORMAT_W(12)
#define STRDEDUP_PERCENT_FORMAT         "%5.1f%%"
#define STRDEDUP_PERCENT_FORMAT_NS      "%.1f%%"
#define STRDEDUP_BYTES_FORMAT           "%8.1f%s"
#define STRDEDUP_BYTES_FORMAT_NS        "%.1f%s"
#define STRDEDUP_BYTES_PARAM(bytes)     byte_size_in_proper_unit((double)(bytes)), proper_unit_for_byte_size((bytes))

#define STRDEDUP_ELAPSED_FORMAT_MS         "%.3fms"
static double strdedup_elapsed_param_ms(Tickspan t) {
  return t.seconds() * MILLIUNITS;
}

StringDedup::Stat::Stat() :
  _inspected(0),
  _skipped(0),
  _known(0),
  _new(0),
  _new_bytes(0),
  _deduped(0),
  _deduped_bytes(0),
  _deleted(0),
  _concurrent(0),
  _idle(0),
  _process(0),
  _rebuild_table(0),
  _cleanup_table(0),
  _block(0),
  _concurrent_start(),
  _concurrent_elapsed(),
  _phase_start(),
  _idle_elapsed(),
  _process_elapsed(),
  _rebuild_table_elapsed(),
  _cleanup_table_elapsed(),
  _block_elapsed() {
}

void StringDedup::Stat::add(const Stat* const stat) {
  _inspected           += stat->_inspected;
  _skipped             += stat->_skipped;
  _known               += stat->_known;
  _new                 += stat->_new;
  _new_bytes           += stat->_new_bytes;
  _deduped             += stat->_deduped;
  _deduped_bytes       += stat->_deduped_bytes;
  _deleted             += stat->_deleted;
  _concurrent          += stat->_concurrent;
  _idle                += stat->_idle;
  _process             += stat->_process;
  _rebuild_table       += stat->_rebuild_table;
  _cleanup_table       += stat->_cleanup_table;
  _block               += stat->_block;
  _concurrent_elapsed  += stat->_concurrent_elapsed;
  _idle_elapsed        += stat->_idle_elapsed;
  _process_elapsed     += stat->_process_elapsed;
  _rebuild_table_elapsed += stat->_rebuild_table_elapsed;
  _cleanup_table_elapsed += stat->_cleanup_table_elapsed;
  _block_elapsed       += stat->_block_elapsed;
}

void StringDedup::Stat::log_summary(const Stat* last_stat, const Stat* total_stat) {
  double total_deduped_bytes_percent = 0.0;

  if (total_stat->_new_bytes > 0) {
    // Avoid division by zero
    total_deduped_bytes_percent = percent_of(total_stat->_deduped_bytes, total_stat->_new_bytes);
  }

  log_info(gc, stringdedup)(
    "Concurrent String Deduplication "
    STRDEDUP_BYTES_FORMAT_NS "->" STRDEDUP_BYTES_FORMAT_NS "(" STRDEDUP_BYTES_FORMAT_NS ") "
    "avg " STRDEDUP_PERCENT_FORMAT_NS ", "
    STRDEDUP_ELAPSED_FORMAT_MS " of " STRDEDUP_ELAPSED_FORMAT_MS,
    STRDEDUP_BYTES_PARAM(last_stat->_new_bytes),
    STRDEDUP_BYTES_PARAM(last_stat->_new_bytes - last_stat->_deduped_bytes),
    STRDEDUP_BYTES_PARAM(last_stat->_deduped_bytes),
    total_deduped_bytes_percent,
    strdedup_elapsed_param_ms(last_stat->_process_elapsed),
    strdedup_elapsed_param_ms(last_stat->_concurrent_elapsed));
}

void StringDedup::Stat::report_concurrent_start() {
  log_debug(gc, stringdedup, phases, start)("Concurrent start");
  _concurrent_start = Ticks::now();
  _concurrent++;
}

void StringDedup::Stat::report_concurrent_end() {
  _concurrent_elapsed += (Ticks::now() - _concurrent_start);
  log_debug(gc, stringdedup, phases)("Concurrent end: " STRDEDUP_ELAPSED_FORMAT_MS,
                                     strdedup_elapsed_param_ms(_concurrent_elapsed));
}

void StringDedup::Stat::report_phase_start(const char* phase) {
  log_debug(gc, stringdedup, phases, start)("%s start", phase);
  _phase_start = Ticks::now();
}

void StringDedup::Stat::report_phase_end(const char* phase, Tickspan* elapsed) {
  *elapsed += Ticks::now() - _phase_start;
  log_debug(gc, stringdedup, phases)("%s end: " STRDEDUP_ELAPSED_FORMAT_MS,
                                     phase, strdedup_elapsed_param_ms(*elapsed));
}

void StringDedup::Stat::report_idle_start() {
  report_phase_start("Idle");
  _idle++;
}

void StringDedup::Stat::report_idle_end() {
  report_phase_end("Idle", &_idle_elapsed);
}

void StringDedup::Stat::report_process_start() {
  report_phase_start("Process");
  _process++;
}

void StringDedup::Stat::report_process_end() {
  report_phase_end("Process", &_process_elapsed);
}

void StringDedup::Stat::report_rebuild_table_start(int new_table_size,
                                                   int old_table_size,
                                                   size_t entry_count,
                                                   size_t dead_count) {
  log_debug(gc, stringdedup, phases, start)
           ("Rebuild Table: %d -> %d (%zu / %zu -> %zu)",
            old_table_size, new_table_size,
            dead_count, entry_count, (entry_count - dead_count));
  _phase_start = Ticks::now();
  _rebuild_table++;
}

void StringDedup::Stat::report_rebuild_table_end() {
  report_phase_end("Rebuild Table", &_rebuild_table_elapsed);
}

void StringDedup::Stat::report_cleanup_table_start(size_t entry_count,
                                                   size_t dead_count) {
  log_debug(gc, stringdedup, phases, start)
           ("Cleanup Table: %zu / %zu -> %zu",
            dead_count, entry_count, (entry_count - dead_count));
  _phase_start = Ticks::now();
  _cleanup_table++;
}

void StringDedup::Stat::report_cleanup_table_end() {
  report_phase_end("Cleanup Table", &_cleanup_table_elapsed);
}

Tickspan* StringDedup::Stat::elapsed_for_phase(Phase phase) {
  switch (phase) {
  case Phase::process: return &_process_elapsed;
  case Phase::rebuild_table: return &_rebuild_table_elapsed;
  case Phase::cleanup_table: return &_cleanup_table_elapsed;
  }
  ShouldNotReachHere();
  return nullptr;
}

void StringDedup::Stat::block_phase(Phase phase) {
  Ticks now = Ticks::now();
  *elapsed_for_phase(phase) += now - _phase_start;
  _phase_start = now;
  _block++;
}

void StringDedup::Stat::unblock_phase() {
  Ticks now = Ticks::now();
  _block_elapsed += now - _phase_start;
  _phase_start = now;
}

void StringDedup::Stat::log_times(const char* prefix) const {
  log_debug(gc, stringdedup)(
    "  %s Process: " UINTX_FORMAT "/" STRDEDUP_ELAPSED_FORMAT_MS
    ", Idle: " UINTX_FORMAT "/" STRDEDUP_ELAPSED_FORMAT_MS
    ", Blocked: " UINTX_FORMAT "/" STRDEDUP_ELAPSED_FORMAT_MS,
    prefix,
    _process, strdedup_elapsed_param_ms(_process_elapsed),
    _idle, strdedup_elapsed_param_ms(_idle_elapsed),
    _block, strdedup_elapsed_param_ms(_block_elapsed));
  if (_rebuild_table > 0) {
    log_debug(gc, stringdedup)(
      "  %s Rebuild Table: " UINTX_FORMAT "/" STRDEDUP_ELAPSED_FORMAT_MS,
      prefix, _rebuild_table, strdedup_elapsed_param_ms(_rebuild_table_elapsed));
  }
  if (_cleanup_table > 0) {
    log_debug(gc, stringdedup)(
      "  %s Cleanup Table: " UINTX_FORMAT "/" STRDEDUP_ELAPSED_FORMAT_MS,
      prefix, _cleanup_table, strdedup_elapsed_param_ms(_cleanup_table_elapsed));
  }
}

void StringDedup::Stat::log_statistics(bool total) const {
  double skipped_percent             = percent_of(_skipped, _inspected);
  double known_percent               = percent_of(_known, _inspected);
  double new_percent                 = percent_of(_new, _inspected);
  double deduped_percent             = percent_of(_deduped, _new);
  double deduped_bytes_percent       = percent_of(_deduped_bytes, _new_bytes);
  double deleted_percent             = percent_of(_deleted, _new);
/*
  double deduped_young_percent       = percent_of(stat._deduped_young, stat._deduped);
  double deduped_young_bytes_percent = percent_of(stat._deduped_young_bytes, stat._deduped_bytes);
  double deduped_old_percent         = percent_of(stat._deduped_old, stat._deduped);
  double deduped_old_bytes_percent   = percent_of(stat._deduped_old_bytes, stat._deduped_bytes);
*/
  log_times(total ? "Total" : "Last");
  log_debug(gc, stringdedup)("    Inspected:    " STRDEDUP_OBJECTS_FORMAT, _inspected);
  log_debug(gc, stringdedup)("      Skipped:    " STRDEDUP_OBJECTS_FORMAT "(" STRDEDUP_PERCENT_FORMAT ")", _skipped, skipped_percent);
  log_debug(gc, stringdedup)("      Known:      " STRDEDUP_OBJECTS_FORMAT "(" STRDEDUP_PERCENT_FORMAT ")", _known, known_percent);
  log_debug(gc, stringdedup)("      New:        " STRDEDUP_OBJECTS_FORMAT "(" STRDEDUP_PERCENT_FORMAT ") " STRDEDUP_BYTES_FORMAT,
                             _new, new_percent, STRDEDUP_BYTES_PARAM(_new_bytes));
  log_debug(gc, stringdedup)("      Deleted:    " STRDEDUP_OBJECTS_FORMAT "(" STRDEDUP_PERCENT_FORMAT ") ", _deleted, deleted_percent);
  log_debug(gc, stringdedup)("    Deduplicated: " STRDEDUP_OBJECTS_FORMAT "(" STRDEDUP_PERCENT_FORMAT ") " STRDEDUP_BYTES_FORMAT "(" STRDEDUP_PERCENT_FORMAT ")",
                             _deduped, deduped_percent, STRDEDUP_BYTES_PARAM(_deduped_bytes), deduped_bytes_percent);
}
