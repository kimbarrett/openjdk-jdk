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

#include "precompiled.hpp"

#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1ConcurrentRefineStats.hpp"
#include "gc/g1/g1ConcurrentRefineThread.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1WrittenCardQueue.hpp"
#include "gc/g1/g1YoungGCPreEvacuateTasks.hpp"
#include "gc/shared/barrierSet.inline.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/threadLocalAllocBuffer.inline.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threads.hpp"

// Flush any partial dirty card buffer in the thread's queue to the global
// list.  Accumulate the flushed card count in stats for later reporting and
// estimating number of cards in thread buffers and so inaccessible to
// concurent refinement.
static void flush_dirty_card_queue(Thread* thread, G1ConcurrentRefineStats& stats) {
  G1DirtyCardQueue& dcq = G1ThreadLocalData::dirty_card_queue(thread);
  if (!dcq.is_empty()) {
    G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
    dcqs.flush_queue(dcq, stats);
  }
}

// Accumulate the per-thread stats and reset the per-thread stats.
static void collect_refinement_stats(Thread* thread,
                                     G1ConcurrentRefineStats& accumulated_stats) {
  G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(thread);
  accumulated_stats += stats;
  stats.reset();
}

class G1PreEvacuateCollectionSetBatchTask::JavaThreadRetireTLABAndFlushLogs : public G1AbstractSubTask {
  G1JavaThreadsListClaimer _claimer;

  // Per worker thread statistics.
  ThreadLocalAllocStats* _local_tlab_stats;
  G1ConcurrentRefineStats* _local_mutator_stats;
  G1ConcurrentRefineStats* _local_flushlogs_stats;

  uint _num_workers;

  // There is relatively little work to do per thread.
  static const uint ThreadsPerWorker = 250;

  struct RetireTLABAndFlushLogsClosure : public ThreadClosure {
    ThreadLocalAllocStats _tlab_stats;
    G1ConcurrentRefineStats _mutator_stats;
    G1ConcurrentRefineStats _flushlogs_stats;

    RetireTLABAndFlushLogsClosure()
      : _tlab_stats(),
        _mutator_stats(),
        _flushlogs_stats()
    {}

    void do_thread(Thread* thread) override {
      assert(thread->is_Java_thread(), "must be");
      // Flushes deferred card marks, so must precede concatenating logs.
      BarrierSet::barrier_set()->make_parsable((JavaThread*)thread);
      if (UseTLAB) {
        thread->tlab().retire(&_tlab_stats);
      }

      if (G1UseWrittenCardQueues) {
        G1WrittenCardQueue& wcq = G1ThreadLocalData::written_card_queue(thread);
        G1DirtyCardQueue& dcq = G1ThreadLocalData::dirty_card_queue(thread);
        _flushlogs_stats.inc_written_cards(wcq.size());
        wcq.mark_cards_dirty(dcq, _flushlogs_stats);
      }
      flush_dirty_card_queue(thread, _flushlogs_stats);
      collect_refinement_stats(thread, _mutator_stats);
    }
  };

  static G1ConcurrentRefineStats
  sum_refinement_stats(const G1ConcurrentRefineStats* stats, uint num_stats) {
    G1ConcurrentRefineStats result{};
    for (uint i = 0; i < num_stats; ++i) {
      result += stats[i];
    }
    return result;
  }

public:
  JavaThreadRetireTLABAndFlushLogs() :
    G1AbstractSubTask(G1GCPhaseTimes::RetireTLABsAndFlushLogs),
    _claimer(ThreadsPerWorker),
    _local_tlab_stats(nullptr),
    _local_mutator_stats(nullptr),
    _local_flushlogs_stats(nullptr),
    _num_workers(0) {
  }

  ~JavaThreadRetireTLABAndFlushLogs() {
    static_assert(std::is_trivially_destructible<G1ConcurrentRefineStats>::value, "must be");
    FREE_C_HEAP_ARRAY(G1ConcurrentRefineStats, _local_mutator_stats);
    FREE_C_HEAP_ARRAY(G1ConcurrentRefineStats, _local_flushlogs_stats);

    static_assert(std::is_trivially_destructible<ThreadLocalAllocStats>::value, "must be");
    FREE_C_HEAP_ARRAY(ThreadLocalAllocStats, _local_tlab_stats);
  }

  void do_work(uint worker_id) override {
    RetireTLABAndFlushLogsClosure tc;
    _claimer.apply(&tc);

    _local_tlab_stats[worker_id] = tc._tlab_stats;

    if (G1UseWrittenCardQueues && G1DeferDirtyingWrittenCards) {
      G1WrittenCardQueueSet& wcqs = G1BarrierSet::written_card_queue_set();
      G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
      G1DirtyCardQueue dcq{&dcqs};
      while (wcqs.mark_cards_dirty(dcq, tc._flushlogs_stats)) {}
      dcqs.flush_queue(dcq, tc._flushlogs_stats);
    }

    _local_mutator_stats[worker_id] = tc._mutator_stats;
    _local_flushlogs_stats[worker_id] = tc._flushlogs_stats;
  }

  double worker_cost() const override {
    return (double)_claimer.length() / ThreadsPerWorker;
  }

  void set_max_workers(uint max_workers) override {
    _num_workers = max_workers;
    _local_tlab_stats = NEW_C_HEAP_ARRAY(ThreadLocalAllocStats, _num_workers, mtGC);
    _local_mutator_stats = NEW_C_HEAP_ARRAY(G1ConcurrentRefineStats, _num_workers, mtGC);
    _local_flushlogs_stats = NEW_C_HEAP_ARRAY(G1ConcurrentRefineStats, _num_workers, mtGC);

    for (uint i = 0; i < _num_workers; i++) {
      ::new (&_local_tlab_stats[i]) ThreadLocalAllocStats();
      ::new (&_local_mutator_stats[i]) G1ConcurrentRefineStats();
      ::new (&_local_flushlogs_stats[i]) G1ConcurrentRefineStats();
    }
  }

  ThreadLocalAllocStats tlab_stats() const {
    ThreadLocalAllocStats result;
    for (uint i = 0; i < _num_workers; i++) {
      result.update(_local_tlab_stats[i]);
    }
    return result;
  }

  G1ConcurrentRefineStats mutator_refinement_stats() const {
    return sum_refinement_stats(_local_mutator_stats, _num_workers);
  }

  G1ConcurrentRefineStats flushlogs_refinement_stats() const {
    return sum_refinement_stats(_local_flushlogs_stats, _num_workers);
  }
};

class G1PreEvacuateCollectionSetBatchTask::NonJavaThreadFlushLogs : public G1AbstractSubTask {
  struct FlushLogsClosure : public ThreadClosure {
    G1ConcurrentRefineStats _mutator_stats;
    G1ConcurrentRefineStats _flushlogs_stats;

    FlushLogsClosure() : _mutator_stats(), _flushlogs_stats() {}

    void do_thread(Thread* thread) override {
      assert(!G1UseWrittenCardQueues ||
             G1ThreadLocalData::written_card_queue(thread).is_empty(),
             "non-Java thread with non-empty written cards queue");
      flush_dirty_card_queue(thread, _flushlogs_stats);
      collect_refinement_stats(thread, _mutator_stats);
    }
  } _tc;

public:
  NonJavaThreadFlushLogs() : G1AbstractSubTask(G1GCPhaseTimes::NonJavaThreadFlushLogs), _tc() { }

  void do_work(uint worker_id) override {
    Threads::non_java_threads_do(&_tc);
  }

  double worker_cost() const override {
    return 1.0;
  }

  G1ConcurrentRefineStats mutator_refinement_stats() const {
    return _tc._mutator_stats;
  }

  G1ConcurrentRefineStats flushlogs_refinement_stats() const {
    return _tc._flushlogs_stats;
  }
};

class G1PreEvacuateCollectionSetBatchTask::ConcurrentRefineThreadFlushLogs : public G1AbstractSubTask {
  struct FlushLogsClosure : public ThreadClosure {
    G1DirtyCardQueueSet& _dcqs;
    G1ConcurrentRefineStats _flushlogs_stats;

    FlushLogsClosure()
      : _dcqs(G1BarrierSet::dirty_card_queue_set()),
        _flushlogs_stats()
    {}

    void do_thread(Thread* thread) override {
      auto crthread = static_cast<G1ConcurrentRefineThread*>(thread);
      _dcqs.flush_queue(crthread->dirty_card_queue(), _flushlogs_stats);
    }
  } _tc;

public:
  ConcurrentRefineThreadFlushLogs()
    : G1AbstractSubTask(G1GCPhaseTimes::ConcurrentRefineThreadFlushLogs),
      _tc()
  {}

  void do_work(uint worker_id) override {
    G1ConcurrentRefine* cr = G1CollectedHeap::heap()->concurrent_refine();
    cr->threads_do(&_tc);
  }

  double worker_cost() const override {
    return 1.0;
  }

  G1ConcurrentRefineStats flushlogs_refinement_stats() const {
    return _tc._flushlogs_stats;
  }
};

G1PreEvacuateCollectionSetBatchTask::G1PreEvacuateCollectionSetBatchTask() :
  G1BatchedTask("Pre Evacuate Prepare", G1CollectedHeap::heap()->phase_times()),
  _java_retire_task(new JavaThreadRetireTLABAndFlushLogs()),
  _non_java_retire_task(new NonJavaThreadFlushLogs()),
  _concurrent_refine_retire_task(nullptr) // Only allocated if needed.
{
  G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();

  // Disable mutator refinement until concurrent refinement decides otherwise.
  if (G1DeferDirtyingWrittenCards) {
    G1WrittenCardQueueSet& wcqs = G1BarrierSet::written_card_queue_set();
    wcqs.set_mutator_should_mark_cards_dirty(false);
  }
  dcqs.set_mutator_refinement_threshold(SIZE_MAX);

  // Flush all paused buffers to the global queue.  Safe from ABA issues here,
  // because we're serially at a safepoint, so there aren't other threads
  // operating on the paused buffer lists or the global queue in the set.
  dcqs.enqueue_all_paused_buffers();

  add_serial_task(_non_java_retire_task);
  if (G1DeferDirtyingWrittenCards) {
    _concurrent_refine_retire_task = new ConcurrentRefineThreadFlushLogs();
    add_serial_task(_concurrent_refine_retire_task);
  }
  add_parallel_task(_java_retire_task);
}

static void verify_empty_dirty_card_logs() {
#ifdef ASSERT
  ResourceMark rm;

  struct Verifier : public ThreadClosure {
    Verifier() {}
    void do_thread(Thread* t) override {
      G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(t);
      assert(queue.is_empty(), "non-empty dirty card queue for thread %s", t->name());
    }
  } verifier;
  Threads::threads_do(&verifier);
#endif
}

G1PreEvacuateCollectionSetBatchTask::~G1PreEvacuateCollectionSetBatchTask() {
  _java_retire_task->tlab_stats().publish();

  verify_empty_dirty_card_logs();

  G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
  G1ConcurrentRefineStats mutator_stats =
    _java_retire_task->mutator_refinement_stats() +
    _non_java_retire_task->mutator_refinement_stats() +
    dcqs.get_and_reset_detached_refinement_stats();

  G1ConcurrentRefineStats flushlogs_stats =
    _java_retire_task->flushlogs_refinement_stats() +
    _non_java_retire_task->flushlogs_refinement_stats();
  if (_concurrent_refine_retire_task != nullptr) {
    flushlogs_stats += _concurrent_refine_retire_task->flushlogs_refinement_stats();
  }

  G1Policy* policy = G1CollectedHeap::heap()->policy();
  policy->record_concurrent_refinement_stats(mutator_stats, flushlogs_stats);

  // Note: The tasks aren't deleted here.  Instead they are deleted by
  // the base class destructor, which deletes all the registered tasks.
}
