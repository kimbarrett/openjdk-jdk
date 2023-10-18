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
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentRefineStats.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1WrittenCardQueue.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/shared/bufferNode.hpp"
#include "gc/shared/gc_globals.hpp"
#include "runtime/atomic.hpp"
#include "runtime/thread.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/prefetch.inline.hpp"
#include "runtime/safepoint.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalCounter.inline.hpp"
#include "utilities/globalDefinitions.hpp"

const size_t NoMatchingCard = SIZE_MAX;

G1WrittenCardQueue::G1WrittenCardQueue() {
  if (G1UseWrittenCardQueues) {
    if (!G1UseInlineWrittenCardBuffers) {
      _indirect._buffer = _indirect._initial;
    }
    size_t cap = current_capacity();
    set_index(cap);
    if (filter_mechanism() == Filter::Previous) {
      (reinterpret_cast<size_t*>(buffer()))[cap] = NoMatchingCard;
    }
  }
}

G1WrittenCardQueue::~G1WrittenCardQueue() {
  assert(!G1UseWrittenCardQueues || is_empty(), "precondition");
  if (G1UseWrittenCardQueues && !G1UseInlineWrittenCardBuffers) {
    if (_indirect._buffer != _indirect._initial) {
      BufferNode* node = BufferNode::make_node_from_buffer(_indirect._buffer);
      G1WrittenCardQueueSet& wcqs = G1BarrierSet::written_card_queue_set();
      wcqs.deallocate_buffer(node);
    }
  }
}

void** G1WrittenCardQueue::buffer() {
  assert(G1UseWrittenCardQueues, "precondition");
  if (G1UseInlineWrittenCardBuffers) {
    return _inline_buffer;
  } else {
    return _indirect._buffer;
  }
}

size_t G1WrittenCardQueue::current_capacity() const {
  assert(G1UseWrittenCardQueues, "precondition");
  size_t capacity;
  if (G1UseInlineWrittenCardBuffers) {
    capacity = ARRAY_SIZE(_inline_buffer);
  } else {
    void** buf = _indirect._buffer;
    if (buf == _indirect._initial) {
      capacity = ARRAY_SIZE(_indirect._initial);
    } else {
      capacity = BufferNode::make_node_from_buffer(buf)->capacity();
    }
  }
  if (filter_mechanism() == Filter::Previous) {
    capacity -= 1;
  }
  return capacity;
}

bool G1WrittenCardQueue::is_empty() const {
  return index() == current_capacity();
}

void G1WrittenCardQueue::reset() {
  set_index(current_capacity());
}

bool G1WrittenCardQueue::mark_cards_dirty(G1DirtyCardQueue& dcq,
                                          G1ConcurrentRefineStats& stats) {
  void** buf;
  size_t capacity;
  if (G1UseInlineWrittenCardBuffers) {
    buf = _inline_buffer;
    capacity = ARRAY_SIZE(_inline_buffer);
  } else {
    buf = _indirect._buffer;
    if (buf == _indirect._initial) {
      capacity = ARRAY_SIZE(_indirect._initial);
    } else {
      capacity = BufferNode::make_node_from_buffer(buf)->capacity();
    }
  }
  size_t idx = index();
  if (capacity > idx) {
    buf += idx;
    size_t size = capacity - idx;
    switch (filter_mechanism()) {
    case Filter::None:
      set_index(capacity);
      return G1WrittenCardQueueSet::mark_cards_dirty_none_filtered(buf, size, dcq, stats);
    case Filter::Young:
      set_index(capacity);
      return G1WrittenCardQueueSet::mark_cards_dirty_young_filtered(buf, size, dcq, stats);
    case Filter::Previous:
      set_index(capacity - 1);
      size -= 1;
      if (size == 0) return false;
      return G1WrittenCardQueueSet::mark_cards_dirty_previous_filtered(buf, size, dcq, stats);
    }
    ShouldNotReachHere();
  } else {
    assert((capacity == idx) && (filter_mechanism() != Filter::Previous), "invariant");
  }
  return false;
}

G1WrittenCardQueueSet::G1WrittenCardQueueSet(BufferNode::Allocator* allocator)
  : _allocator(allocator),
    _mutator_should_mark_cards_dirty(false),
    _num_cards(0),
    _buffer_list()
{}

G1WrittenCardQueueSet::~G1WrittenCardQueueSet() {
  abandon_completed_buffers();
}

size_t G1WrittenCardQueueSet::num_cards() const {
  return Atomic::load(&_num_cards);
}

void G1WrittenCardQueueSet::abandon_completed_buffers() {
  assert_at_safepoint();
  BufferNode* buffers_to_delete = _buffer_list.pop_all();
  while (buffers_to_delete != nullptr) {
    BufferNode* bn = buffers_to_delete;
    buffers_to_delete = bn->next();
    bn->set_next(nullptr);
    deallocate_buffer(bn);
  }
  Atomic::store(&_num_cards, size_t(0));
}

void G1WrittenCardQueueSet::enqueue_completed_buffer(BufferNode* node) {
  assert(G1DeferDirtyingWrittenCards, "precondition");
  assert(node != nullptr, "precondition");
  // Increment count before pushing, so count is always at least actual and
  // decrement during take never underflows.
  Atomic::add(&_num_cards, node->size());
  _buffer_list.push(*node);
}

BufferNode* G1WrittenCardQueueSet::take_completed_buffer() {
  BufferNode* node;
  {
    GlobalCounter::CriticalSection cs(Thread::current());
    node = _buffer_list.pop();
  }
  if (node != nullptr) {
    Atomic::sub(&_num_cards, node->size());
  }
  return node;
}

bool G1WrittenCardQueueSet::mark_cards_dirty(G1DirtyCardQueue& dcq,
                                             G1ConcurrentRefineStats& stats) {
  BufferNode* node = take_completed_buffer();
  if (node == nullptr) return false;
  assert(!node->is_empty(), "empty completed written buffer");
  void** buf = BufferNode::make_buffer_from_node(node) + node->index();
  size_t size = node->size();
  switch (filter_mechanism()) {
  case Filter::None:
    mark_cards_dirty_none_filtered(buf, size, dcq, stats);
    break;
  case Filter::Young:
    mark_cards_dirty_young_filtered(buf, size, dcq, stats);
    break;
  case Filter::Previous:
    mark_cards_dirty_previous_filtered(buf, size - 1, dcq, stats);
    break;
  default:
    ShouldNotReachHere();
  }
  deallocate_buffer(node);
  return true;
}

void** G1WrittenCardQueueSet::allocate_buffer() {
  BufferNode* node = _allocator->allocate();
  return BufferNode::make_buffer_from_node(node);
}

void G1WrittenCardQueueSet::deallocate_buffer(BufferNode* node) {
  _allocator->release(node);
}

// mark_cards_dirty_XXX_filtered applies the XXX filter to the written card
// information in the size-limited written buffer, transforming the contents
// of the written buffer into CardValue* values.
//
// We don't do written-card processing time tracking here.  The cost of time
// tracking is relatively high compared to the processing, because clock
// access may not be super fast, and written card processing is very time
// critical.

bool G1WrittenCardQueueSet::mark_cards_dirty_none_filtered(void** written,
                                                           size_t size,
                                                           G1DirtyCardQueue& dcq,
                                                           G1ConcurrentRefineStats& stats) {
  assert(G1UseWrittenCardQueues, "precondition");
  assert(filter_mechanism() == Filter::None, "precondition");
  G1CardTable* ct = G1CollectedHeap::heap()->card_table();
  G1CardTable::CardValue* ct_base = ct->byte_map_base();
  size_t previous = SIZE_MAX;   // Doesn't match any valid card index.
  size_t kept = 0;
  for (size_t i = 0; i < size; ++i) {
    // Transform a written address into a card index.
    size_t card = reinterpret_cast<size_t>(written[i]) >> G1CardTable::card_shift();
    // Drop sequential runs of the same card.
    if (previous == card) continue;
    previous = card;
    // Transform a card index into a CardValue*, and store back into buffer
    // for further processing later.
    G1CardTable::CardValue* p = ct_base + card;
    Prefetch::read(p, 0);       // We're going to be reading it soon.
    written[kept++] = p;
  }
  stats.inc_written_cards_filtered(size - kept);
  return enqueue_clean_cards_helper(written, kept, dcq, stats);
}

bool G1WrittenCardQueueSet::mark_cards_dirty_young_filtered(void** written,
                                                            size_t size,
                                                            G1DirtyCardQueue& dcq,
                                                            G1ConcurrentRefineStats& stats) {
  assert(G1UseWrittenCardQueues, "precondition");
  assert(filter_mechanism() == Filter::Young, "precondition");
  // The written buffer already contains CardValue*'s that have "recently"
  // been read to check for younggen marker.  No further setup needed.
  return enqueue_clean_cards_helper(written, size, dcq, stats);
}

bool G1WrittenCardQueueSet::mark_cards_dirty_previous_filtered(void** written,
                                                               size_t size,
                                                               G1DirtyCardQueue& dcq,
                                                               G1ConcurrentRefineStats& stats) {
  assert(G1UseWrittenCardQueues, "precondition");
  assert(filter_mechanism() == Filter::Previous, "precondition");
  G1CardTable* ct = G1CollectedHeap::heap()->card_table();
  G1CardTable::CardValue* ct_base = ct->byte_map_base();
  for (size_t i = 0; i < size; ++i) {
    // Transform a written address into a CardValue*, and store back into
    // buffer for further processing later.  Unlike the "none" filter, here
    // we've already dropped sequential runs of the same card.
    G1CardTable::CardValue* p = ct_base + reinterpret_cast<size_t>(written[i]);
    Prefetch::read(p, 0);       // We're going to be reading it soon.
    written[i] = p;
  }
  return enqueue_clean_cards_helper(written, size, dcq, stats);
}

bool G1WrittenCardQueueSet::enqueue_clean_cards_helper(void** written,
                                                       size_t size,
                                                       G1DirtyCardQueue& dcq,
                                                       G1ConcurrentRefineStats& stats) {
  return ((size > 0) &&
          enqueue_clean_cards(reinterpret_cast<CardValue**>(written),
                              size,
                              dcq,
                              stats));
}

bool G1WrittenCardQueueSet::enqueue_clean_cards(CardValue** written,
                                                size_t size,
                                                G1DirtyCardQueue& dcq,
                                                G1ConcurrentRefineStats& stats) {
  bool flushed = false;
  size_t dirtied = 0;
  size_t filtered = 0;
  void** dirty_buffer = dcq.buffer();
  size_t dirty_index = dcq.index();
  for (size_t i = 0; i < size; ++i) {
    CardValue* p = written[i];
    if (Atomic::load(p) != G1CardTable::clean_card_val()) {
      filtered += 1;
    } else {
      // Card is clean, so set to dirty and enqueue the card in dcq.
      Atomic::store(p, G1CardTable::dirty_card_val());
      dirtied += 1;
      if (dirty_index > 0) {
        // Bulk enqueue, with index update deferred, rather than going through
        // the generic enqueue operation.  This knows too much about the dirty
        // card queue implementation.
        dirty_buffer[--dirty_index] = p;
      } else {
        // Queue is full.  Do a normal enqueue operation, which will deal with
        // the full buffer and then add the card.
        dcq.set_index(dirty_index);
        G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
        dcqs.enqueue(dcq, p, stats);
        dirty_buffer = dcq.buffer();
        dirty_index = dcq.index();
        flushed = true;
      }
    }
  }
  assert(dirtied + filtered == size, "invariant");
  stats.inc_written_cards_dirtied(dirtied);
  stats.inc_written_cards_filtered(filtered);
  // Finish recent bulk enqueues.
  dcq.set_index(dirty_index);
  return flushed;
}

inline bool G1WrittenCardQueueSet::mutator_should_mark_cards_dirty() const {
  return Atomic::load(&_mutator_should_mark_cards_dirty);
}

void G1WrittenCardQueueSet::set_mutator_should_mark_cards_dirty(bool value) {
  Atomic::store(&_mutator_should_mark_cards_dirty, value);
}

template<size_t size_adjust, typename Marker>
void G1WrittenCardQueueSet::handle_full_buffer_inline(Thread* t, Marker marker) {
  G1WrittenCardQueue& wcq = G1ThreadLocalData::written_card_queue(t);
  assert(wcq.index() == 0, "written card queue not full");
  G1DirtyCardQueue& dcq = G1ThreadLocalData::dirty_card_queue(t);
  G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(t);
  void** buffer = wcq._inline_buffer;
  size_t bufsize = ARRAY_SIZE(wcq._inline_buffer) - size_adjust;
  stats.inc_written_cards(bufsize);
  wcq.set_index(bufsize);
  // The stores being tracked must happen-before the conditional dirty marking.
  OrderAccess::fence();
  if (marker(buffer, bufsize, dcq, stats)) {
    G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
    dcqs.mutator_refine_completed_buffer(stats);
  }
}

void G1WrittenCardQueueSet::handle_full_buffer_inline_none(Thread* t) {
  handle_full_buffer_inline<0>(t, &mark_cards_dirty_none_filtered);
}

void G1WrittenCardQueueSet::handle_full_buffer_inline_young(Thread* t) {
  handle_full_buffer_inline<0>(t, &mark_cards_dirty_young_filtered);
}

void G1WrittenCardQueueSet::handle_full_buffer_inline_previous(Thread* t) {
  handle_full_buffer_inline<1>(t, &mark_cards_dirty_previous_filtered);
}

bool G1WrittenCardQueueSet::handle_full_indirect_initial_buffer(G1WrittenCardQueue& wcq,
                                                                void** buffer) {
  if (buffer != wcq._indirect._initial) return false;
  G1WrittenCardQueueSet& wcqs = G1BarrierSet::written_card_queue_set();
  void** new_buffer = wcqs.allocate_buffer();
  size_t index = BufferNode::make_node_from_buffer(new_buffer)->capacity();
  index -= ARRAY_SIZE(wcq._indirect._initial);
  for (size_t i = 0; i < ARRAY_SIZE(wcq._indirect._initial); ++i) {
    new_buffer[index + i] = buffer[i];
  }
  wcq._indirect._buffer = new_buffer;
  wcq.set_index(index);
  return true;
}

template<size_t size_adjust, typename Marker>
void G1WrittenCardQueueSet::handle_full_buffer_indirect(Thread* t, Marker marker) {
  G1WrittenCardQueue& wcq = G1ThreadLocalData::written_card_queue(t);
  assert(wcq.index() == 0, "written card queue not full");
  void** buffer = wcq._indirect._buffer;
  if (handle_full_indirect_initial_buffer(wcq, buffer)) return;
  G1DirtyCardQueue& dcq = G1ThreadLocalData::dirty_card_queue(t);
  G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(t);
  BufferNode* node = BufferNode::make_node_from_buffer(buffer);
  size_t bufsize = node->capacity() - size_adjust;
  stats.inc_written_cards(bufsize);
  wcq.set_index(bufsize);
  // The stores being tracked must happen-before the conditional dirty marking.
  OrderAccess::fence();
  if (marker(buffer, bufsize, dcq, stats)) {
    G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
    dcqs.mutator_refine_completed_buffer(stats);
  }
}

void G1WrittenCardQueueSet::handle_full_buffer_indirect_none(Thread* t) {
  handle_full_buffer_indirect<0>(t, &mark_cards_dirty_none_filtered);
}

void G1WrittenCardQueueSet::handle_full_buffer_indirect_young(Thread* t) {
  handle_full_buffer_indirect<0>(t, &mark_cards_dirty_young_filtered);
}

void G1WrittenCardQueueSet::handle_full_buffer_indirect_previous(Thread* t) {
  handle_full_buffer_indirect<1>(t, &mark_cards_dirty_previous_filtered);
}

template<size_t size_adjust, typename Marker>
void G1WrittenCardQueueSet::handle_full_buffer_deferred(Thread* t, Marker marker) {
  G1WrittenCardQueue& wcq = G1ThreadLocalData::written_card_queue(t);
  assert(wcq.index() == 0, "written card queue not full");
  G1WrittenCardQueueSet& wcqs = G1BarrierSet::written_card_queue_set();
  if (wcqs.mutator_should_mark_cards_dirty()) {
    handle_full_buffer_indirect<size_adjust>(t, marker);
    return;
  }
  void** buffer = wcq._indirect._buffer;
  if (handle_full_indirect_initial_buffer(wcq, buffer)) return;

  void** new_buffer = wcqs.allocate_buffer();
  BufferNode* new_node = BufferNode::make_node_from_buffer(new_buffer);
  size_t bufsize = new_node->capacity() - size_adjust;
  BufferNode* old_node = BufferNode::make_node_from_buffer(buffer, 0);
  G1ConcurrentRefineStats& stats = G1ThreadLocalData::refinement_stats(t);
  stats.inc_written_cards(old_node->size());
  wcqs.enqueue_completed_buffer(old_node);
  wcq._indirect._buffer = new_buffer;
  wcq.set_index(bufsize);
  if (size_adjust != 0) {
    assert(size_adjust == 1, "unexpected size adjustment value");
    assert(filter_mechanism() == Filter::Previous, "unexpected size adjustment");
    (reinterpret_cast<size_t*>(new_buffer))[bufsize] = NoMatchingCard;
  }
}

void G1WrittenCardQueueSet::handle_full_buffer_deferred_none(Thread* t) {
  handle_full_buffer_deferred<0>(t, &mark_cards_dirty_none_filtered);
}

void G1WrittenCardQueueSet::handle_full_buffer_deferred_young(Thread* t) {
  handle_full_buffer_deferred<0>(t, &mark_cards_dirty_young_filtered);
}

void G1WrittenCardQueueSet::handle_full_buffer_deferred_previous(Thread* t) {
  handle_full_buffer_deferred<1>(t, &mark_cards_dirty_previous_filtered);
}
