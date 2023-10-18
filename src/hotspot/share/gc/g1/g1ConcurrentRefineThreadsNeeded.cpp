/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1Analytics.hpp"
#include "gc/g1/g1ConcurrentRefineThreadsNeeded.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/shared/gc_globals.hpp"
#include "utilities/globalDefinitions.hpp"
#include <math.h>

G1ConcurrentRefineThreadsNeeded::G1ConcurrentRefineThreadsNeeded(G1Policy* policy,
                                                                 double update_period_ms) :
  _policy(policy),
  _update_period_ms(update_period_ms),
  _predicted_time_until_next_gc_ms(0.0),
  _predicted_written_cards_at_next_gc(0),
  _predicted_dirty_cards_at_next_gc(0),
  _written_cards_deactivation_threshold(0),
  _threads_needed(0)
{}

// Estimate how many concurrent refinement threads we need to run to achieve
// the target number of card by the time the next GC happens.  There are
// several secondary goals we'd like to achieve while meeting that goal.
//
// 1. Minimize the number of refinement threads running at once.
//
// 2. Minimize the number of activations and deactivations for the
// refinement threads that run.
//
// 3. Delay performing refinement work.  Having more dirty cards waiting to
// be refined can be beneficial, as further writes to the same card don't
// create more work.
void G1ConcurrentRefineThreadsNeeded::update(uint active_threads,
                                             size_t available_bytes,
                                             size_t num_written_cards,
                                             size_t num_dirty_cards,
                                             size_t target_num_dirty_cards) {
  const G1Analytics* analytics = _policy->analytics();

  // Estimate time until next GC, based on remaining bytes available for
  // allocation and the allocation rate.
  double alloc_region_rate = analytics->predict_alloc_rate_ms();
  double alloc_bytes_rate = alloc_region_rate * HeapRegion::GrainBytes;
  if (alloc_bytes_rate == 0.0) {
    // A zero rate indicates we don't yet have data to use for predictions.
    // Since we don't have any idea how long until the next GC, use a time of
    // zero.
    _predicted_time_until_next_gc_ms = 0.0;
  } else {
    // If the heap size is large and the allocation rate is small, we can get
    // a predicted time until next GC that is so large it can cause problems
    // (such as overflow) in other calculations.  Limit the prediction to one
    // hour, which is still large in this context.
    const double one_hour_ms = 60.0 * 60.0 * MILLIUNITS;
    double raw_time_ms = available_bytes / alloc_bytes_rate;
    _predicted_time_until_next_gc_ms = MIN2(raw_time_ms, one_hour_ms);
  }

  // Estimate the number of cards (written or dirtied) at the next GC if no
  // further processing (dirtying or refinement respectively) is performed.
  double incoming_written_rate = analytics->predict_written_cards_rate_ms();
  size_t total_written_cards = predict_cards_at_next_gc(num_written_cards, incoming_written_rate);
  _predicted_written_cards_at_next_gc = total_written_cards;

  double incoming_dirty_rate = analytics->predict_dirtied_cards_rate_ms();
  size_t total_dirty_cards = predict_cards_at_next_gc(num_dirty_cards, incoming_dirty_rate);
  _predicted_dirty_cards_at_next_gc = total_dirty_cards;

  // Start with the deactivation limit set to not deactivate if there are any
  // written cards to be processed.  We may update it later if we have
  // sufficient data to choose a better value.
  _written_cards_deactivation_threshold = 0;

  // The calculation of the number of threads needed isn't very stable when
  // time is short, and can lead to starting up lots of threads for not much
  // profit.  If we're in the last update period, don't change the number of
  // threads running, other than to treat the current thread as running.  That
  // might not be sufficient, but hopefully we were already reasonably close.
  // We won't accumulate more written cards because mutator dirtying will be
  // activated.  Mutator refinement will also be activated, so we won't
  // accumulate dirty cards from mutator threads, though we can get some from
  // dirtying deferred written cards by refinement threads.
  if (_predicted_time_until_next_gc_ms <= _update_period_ms) {
    _threads_needed = MAX2(active_threads, 1u);
    return;
  }

  // Estimate the rate at which a thread can process cards.  If neither of
  // them have estimates available yet (values are 0) then just request one
  // running thread.  Just one thread might not be sufficient, but we don't
  // have any idea how many we actually need.  And we need to do some
  // processing in order to gain those estimates.  Eventually the prediction
  // machinery will warm up and we'll be able to get estimates.
  double dirtying_rate = analytics->predict_concurrent_dirtying_rate_ms();
  double refine_rate = analytics->predict_concurrent_refine_rate_ms();
  if ((dirtying_rate == 0.0) && (refine_rate == 0.0)) {
    _threads_needed = 1;
    return;
  }

  // Accumulator for the number of threads needed for various kinds of processing.
  double nthreads = 0;

  // Estimate the number of cards that need to be refined before the next GC
  // to meet the goal.
  size_t cards_to_refine = 0;
  if (total_dirty_cards > target_num_dirty_cards) {
    cards_to_refine = total_dirty_cards - target_num_dirty_cards;
  }

  // Estimate the number of threads performing refinement we need to run in
  // order to reach the goal in time.
  if (cards_to_refine > 0) {
    if (refine_rate == 0.0) {
      // If we don't have an estimate then request one running thread to cover
      // this part of the processing.  That might not be sufficient, but we
      // don't have any idea how many we actually need.  Eventually the
      // prediction machinary will warm up and we'll be able to get estimates.
      nthreads += 1.0;
    } else {
      nthreads += estimate_threads_needed(cards_to_refine, refine_rate);
    }
  }

  // Estimate the number of threads performing written card dirtying we need
  // to run in order to reach the goal in time.
  if (G1DeferDirtyingWrittenCards) {
    // Set the deactivation limit to the number of cards that can be processed
    // by one thread in half an update period.  (The factor of 1/2 is not
    // carefully chose.)  The controller may reduce the number of active
    // threads when the refinement goal has been met and the number of written
    // cards is below this value.  If there are lots of pending written cards
    // then we want to keep active threads running to quickly drive that
    // number down.
    _written_cards_deactivation_threshold =
      static_cast<size_t>(dirtying_rate * (_update_period_ms / 2.0));
    if (dirtying_rate == 0.0) {
      // If we don't have an estimate then request one running thread to cover
      // this part of the processing.  That might not be sufficient, but we
      // don't have any idea how many we actually need.  Eventually the
      // prediction machinary will warm up and we'll be able to get estimates.
      nthreads += 1.0;
    } else {
      // We want to drive the number of pending written cards to (near) zero
      // and keep it there.  Written cards are very cheap to process,
      // producing some lesser number of dirty cards.  The main driver for
      // needing refinement threads is the number of dirty cards needing
      // refinement.  So having few written cards pending possible conversion
      // to dirty cards improves our estimates of work to be done.
      //
      // However, we also want to keep the number of running refinement
      // processes low, to minimize interference with running mutator threads.
      //
      // So we use several heuristics to come up with a candidate number of
      // threads, and use the minimum among them.

      // Estimated minimum number of continuously running threads needed to
      // process all the written cards before the next GC.
      double minimum = estimate_threads_needed(total_written_cards, dirtying_rate);

      // Estimated number of threads needed to get the number of pending
      // written cards to (near) zero in one update period.
      double period_capacity = dirtying_rate * _update_period_ms;
      double period_incoming = incoming_dirty_rate * _update_period_ms;
      double period_target = static_cast<double>(num_written_cards) + period_incoming;
      double period_threads = period_target / period_capacity;

      nthreads += MIN3(minimum + 1.0, 2.0 * minimum, period_threads);
    }
  }

  // Decide how to round nthreads to an integral number of threads.  Always
  // rounding up is contrary to delaying refinement work.  Usually round to
  // the nearest.  But when we're close to the next GC we want to drive toward
  // the target, so round up then.  And as a special case, always use at least
  // one.  The current thread is the primary refinement thread, which is
  // already running.  We can just let it proceed and automatically deactivate
  // itself if it runs out of work, possibly right away.
  if (nthreads <= 1.0) {
    nthreads = 1.0;
  } else if (_predicted_time_until_next_gc_ms <= _update_period_ms * 5.0) {
    nthreads = ::ceil(nthreads);
  } else {
    nthreads = ::round(nthreads);
  }

  _threads_needed = static_cast<uint>(MIN2<size_t>(nthreads, UINT_MAX));
}

size_t G1ConcurrentRefineThreadsNeeded::predict_cards_at_next_gc(size_t num_cards,
                                                                 double incoming_rate_ms) const {
  size_t incoming_cards =
    static_cast<size_t>(incoming_rate_ms * _predicted_time_until_next_gc_ms);
  return num_cards + incoming_cards;
}

double G1ConcurrentRefineThreadsNeeded::estimate_threads_needed(size_t num_cards,
                                                                double processing_rate_ms) const {
  double thread_capacity = processing_rate_ms * _predicted_time_until_next_gc_ms;
  return static_cast<double>(num_cards) / thread_capacity;
}
