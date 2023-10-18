/*
 * Copyright (c) 2020, 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1CONCURRENTREFINESTATS_HPP
#define SHARE_GC_G1_G1CONCURRENTREFINESTATS_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ticks.hpp"

// Collection of statistics for concurrent refinement processing.
// Used for collecting per-thread statistics and for summaries over a
// collection of threads.
class G1ConcurrentRefineStats : public CHeapObj<mtGC> {
  Tickspan _refinement_time;
  size_t _refined_cards;
  size_t _precleaned_cards;
  size_t _dirtied_cards;
  Tickspan _written_cards_processing_time;
  size_t _written_cards_dirtied;
  size_t _written_cards_filtered;
  size_t _written_cards;

public:
  G1ConcurrentRefineStats();

  // Time spent performing concurrent refinement.
  Tickspan refinement_time() const { return _refinement_time; }

  // Number of refined cards.
  size_t refined_cards() const { return _refined_cards; }

  // Refinement rate, in cards per ms.
  double refinement_rate_ms() const;

  // Number of cards for which refinement was skipped because some other
  // thread had already refined them.
  size_t precleaned_cards() const { return _precleaned_cards; }

  // Number of cards marked dirty and in need of refinement.
  size_t dirtied_cards() const { return _dirtied_cards; }

  // All the stats related to written cards are only used when
  // G1UseWrittenCardQueues is true.

  // Time spent processing written cards.
  Tickspan written_cards_processing_time() const {
    return _written_cards_processing_time;
  }

  // Number of written cards that were dirtied.
  size_t written_cards_dirtied() const { return _written_cards_dirtied; }

  // Number of processed written cards that were discarded by filtering.
  size_t written_cards_filtered() const { return _written_cards_filtered; }

  // Number of written cards processed, when G1UseWrittenCardQueues is true;
  // This is the sum of the number of cards dirtied or filtered.
  size_t written_cards_processed() const {
    return written_cards_dirtied() + written_cards_filtered();
  }

  // Number of written cards, when G1UseWrittenCardQueues is true.
  size_t written_cards() const { return _written_cards; }

  // Processing rate, in cards per ms, when G1UseWrittenCardQueues is true.
  double written_cards_processing_rate_ms() const;

  void inc_refinement_time(Tickspan t) { _refinement_time += t; }
  void inc_refined_cards(size_t cards) { _refined_cards += cards; }
  void inc_precleaned_cards(size_t cards) { _precleaned_cards += cards; }
  void inc_dirtied_cards(size_t cards) { _dirtied_cards += cards; }
  void inc_written_cards_processing_time(Tickspan t) { _written_cards_processing_time += t; }
  void inc_written_cards_dirtied(size_t cards) { _written_cards_dirtied += cards; }
  void inc_written_cards_filtered(size_t cards) { _written_cards_filtered += cards; }
  void inc_written_cards(size_t cards) { _written_cards += cards; }

  G1ConcurrentRefineStats& operator+=(const G1ConcurrentRefineStats& other);
  G1ConcurrentRefineStats& operator-=(const G1ConcurrentRefineStats& other);

  friend G1ConcurrentRefineStats operator+(G1ConcurrentRefineStats x,
                                           const G1ConcurrentRefineStats& y) {
    return x += y;
  }

  friend G1ConcurrentRefineStats operator-(G1ConcurrentRefineStats x,
                                           const G1ConcurrentRefineStats& y) {
    return x -= y;
  }

  void reset();
};

#endif // SHARE_GC_G1_G1CONCURRENTREFINESTATS_HPP
