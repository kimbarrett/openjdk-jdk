/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1BarrierSet.inline.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1CardTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1SATBMarkQueueSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1WrittenCardQueue.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "logging/log.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/threads.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif

class G1BarrierSetC1;
class G1BarrierSetC2;
class G1ConcurrentRefineStats;

G1BarrierSet::G1BarrierSet(G1CardTable* card_table) :
  CardTableBarrierSet(make_barrier_set_assembler<G1BarrierSetAssembler>(),
                      make_barrier_set_c1<G1BarrierSetC1>(),
                      make_barrier_set_c2<G1BarrierSetC2>(),
                      card_table,
                      BarrierSet::FakeRtti(BarrierSet::G1BarrierSet)),
  _satb_mark_queue_buffer_allocator("SATB Buffer Allocator", G1SATBBufferSize),
  _written_card_queue_buffer_allocator("WC Buffer Allocator", G1WrittenCardBufferSize),
  _dirty_card_queue_buffer_allocator("DC Buffer Allocator", G1UpdateBufferSize),
  _satb_mark_queue_set(&_satb_mark_queue_buffer_allocator),
  _written_card_queue_set(&_written_card_queue_buffer_allocator),
  _dirty_card_queue_set(&_dirty_card_queue_buffer_allocator)
{}

template <class T> void
G1BarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(Thread::current());

  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      queue_set.enqueue_known_active(queue, CompressedOops::decode_not_null(heap_oop));
    }
  }
}

void G1BarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_field_post_slow(volatile CardValue* byte) {
  // In the slow path, we know a card is not young
  assert(*byte != G1CardTable::g1_young_card_val(), "slow path invoked without filtering");
  OrderAccess::storeload();
  if (*byte != G1CardTable::dirty_card_val()) {
    *byte = G1CardTable::dirty_card_val();
    Thread* thr = Thread::current();
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thr);
    G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(thr);
    G1BarrierSet::dirty_card_queue_set().enqueue(queue, byte, stats);
  }
}

void G1BarrierSet::invalidate(JavaThread* thread, MemRegion mr) {
  if (mr.is_empty()) {
    return;
  }
  volatile CardValue* byte = _card_table->byte_for(mr.start());
  CardValue* last_byte = _card_table->byte_for(mr.last());

  // skip young gen cards
  if (*byte == G1CardTable::g1_young_card_val()) {
    // MemRegion should not span multiple regions for the young gen.
    DEBUG_ONLY(HeapRegion* containing_hr = G1CollectedHeap::heap()->heap_region_containing(mr.start());)
    assert(containing_hr->is_young(), "it should be young");
    assert(containing_hr->is_in(mr.start()), "it should contain start");
    assert(containing_hr->is_in(mr.last()), "it should also contain last");
    return;
  }

  OrderAccess::storeload();
  // Enqueue if necessary.
  G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
  G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thread);
  G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(thread);
  for (; byte <= last_byte; byte++) {
    CardValue bv = *byte;
    assert(bv != G1CardTable::g1_young_card_val(), "Invalid card");
    if (bv != G1CardTable::dirty_card_val()) {
      *byte = G1CardTable::dirty_card_val();
      qset.enqueue(queue, byte, stats);
    }
  }
}

void G1BarrierSet::on_thread_create(Thread* thread) {
  // Create thread local data
  G1ThreadLocalData::create(thread);
}

void G1BarrierSet::on_thread_destroy(Thread* thread) {
  // Destroy thread local data
  G1ThreadLocalData::destroy(thread);
}

void G1BarrierSet::on_thread_attach(Thread* thread) {
  BarrierSet::on_thread_attach(thread);
  SATBMarkQueue& satbq = G1ThreadLocalData::satb_mark_queue(thread);
  assert(!satbq.is_active(), "SATB queue should not be active");
  assert(satbq.buffer() == nullptr, "SATB queue should not have a buffer");
  assert(satbq.index() == 0, "SATB queue index should be zero");
  if (G1UseWrittenCardQueues) {
    G1WrittenCardQueue& writtenq = G1ThreadLocalData::written_card_queue(thread);
    assert(writtenq.is_empty(), "Written Card queue should be empty");
  }
  G1DirtyCardQueue& dirtyq = G1ThreadLocalData::dirty_card_queue(thread);
  assert(dirtyq.buffer() == nullptr, "Dirty Card queue should not have a buffer");
  assert(dirtyq.index() == 0, "Dirty Card queue index should be zero");

  // If we are creating the thread during a marking cycle, we should
  // set the active field of the SATB queue to true.  That involves
  // copying the global is_active value to this thread's queue.
  satbq.set_active(_satb_mark_queue_set.is_active());
}

void G1BarrierSet::on_thread_detach(Thread* thread) {
  // Flush any deferred card marks.
  CardTableBarrierSet::on_thread_detach(thread);
  {
    SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
    G1BarrierSet::satb_mark_queue_set().flush_queue(queue);
  }
  if (G1UseWrittenCardQueues) {
    G1WrittenCardQueue& wcq = G1ThreadLocalData::written_card_queue(thread);
    G1DirtyCardQueue& dcq = G1ThreadLocalData::dirty_card_queue(thread);
    G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(thread);
    wcq.mark_cards_dirty(dcq, stats);
    if (!G1UseInlineWrittenCardBuffers) {
      // FIXME: Kludgy way to discard buffer without additional API.
      wcq.~G1WrittenCardQueue();
      ::new (&wcq) G1WrittenCardQueue();
    }
  }
  {
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thread);
    G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(thread);
    G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
    qset.flush_queue(queue, stats);
    qset.record_detached_refinement_stats(stats);
  }
}

void G1BarrierSet::abandon_post_barrier_logs_and_stats() {
  assert_at_safepoint();

  G1BarrierSet* bs = g1_barrier_set();
  G1DirtyCardQueueSet& dcqs = bs->dirty_card_queue_set();

  struct AbandonClosure : public ThreadClosure {
    G1DirtyCardQueueSet& _dcqs;
    AbandonClosure(G1DirtyCardQueueSet& dcqs) : _dcqs(dcqs) {}
    void do_thread(Thread* t) override {
      if (G1UseWrittenCardQueues) {
        G1ThreadLocalData::written_card_queue(t).reset();
      }
      _dcqs.reset_queue(G1ThreadLocalData::dirty_card_queue(t));
      G1ThreadLocalData::refinement_stats(t).reset();
    }
  } closure(dcqs);
  Threads::threads_do(&closure);

  if (G1UseWrittenCardQueues) {
    bs->written_card_queue_set().abandon_completed_buffers();
  }
  bs->dirty_card_queue_set().abandon_completed_buffers_and_stats();
}
