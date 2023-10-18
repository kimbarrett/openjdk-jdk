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

#ifndef SHARE_GC_G1_WRITTENCARDQUEUE_HPP
#define SHARE_GC_G1_WRITTENCARDQUEUE_HPP

#include "gc/shared/bufferNode.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "memory/padded.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/lockFreeStack.hpp"
#include "utilities/macros.hpp"
#include "utilities/sizes.hpp"

extern "C" uint G1WrittenCardFilter;

class G1WrittenCardQueueSet;

// A queue for collecting written card information.  The layout depends on the
// value of G1UseInlineWrittenCardBuffers.  The values in the queue depend on
// the value of G1WrittenCardFilter:
//
// 0: No filtering by the caller.  Queue values are the addresses written to,
// after applying barrier precision.
//
// 1: The caller filters out writes to the young generation, and records the
// card table pointer corresponding to the address written to.
//
// 2: The caller filters out sequential writes to the same card, and records
// the card index for the address written to, i.e. the address right shifted
// by the log of card size.

class G1WrittenCardQueue {
  friend class G1WrittenCardQueueSet;

public:
  enum class Filter {
    None = 0,                   // No filtering.
    Young = 1,                  // Filter out young cards.
    Previous = 2,               // Filter out duplicates in sequence.
  };

  G1WrittenCardQueue();
  ~G1WrittenCardQueue();

  NONCOPYABLE(G1WrittenCardQueue);

  void** buffer();

  size_t current_capacity() const;

  size_t index() const {
    return byte_index_to_index(_index_in_bytes);
  }

  void set_index(size_t new_index) {
    assert(new_index <= current_capacity(), "precondition");
    _index_in_bytes = index_to_byte_index(new_index);
  }

  size_t size() const {
    return current_capacity() - index();
  }

  bool is_empty() const;

  void reset();

  static Filter filter_mechanism() {
    return static_cast<Filter>(G1WrittenCardFilter);
  }

  // Mark cards in the written queue dirty.
  // Returns true if the dcq was flushed because of a full buffer.
  bool mark_cards_dirty(G1DirtyCardQueue& dcq, G1ConcurrentRefineStats& stats);

  // Compiler support.
  static ByteSize byte_offset_of_index() {
    return byte_offset_of(G1WrittenCardQueue, _index_in_bytes);
  }

  static ByteSize byte_offset_of_inline_buffer() {
    return byte_offset_of(G1WrittenCardQueue, _inline_buffer);
  }

  static ByteSize byte_offset_of_indirect_buffer() {
    return byte_offset_of(G1WrittenCardQueue, _indirect._buffer);
  }

private:
  // The byte index at which an object was last enqueued.  Starts at capacity
  // (in bytes), indicating an empty buffer, and goes towards zero.  Value is
  // always pointer-size aligned.
  size_t _index_in_bytes;

  union {
    void* _inline_buffer[36];
    struct {
      void** _buffer;
      void* _initial[2];
    } _indirect;
  };

  static const size_t _element_size = sizeof(void*);

  static size_t byte_index_to_index(size_t i) {
    assert(is_aligned(i, _element_size), "precondition");
    return i / _element_size;
  }

  static size_t index_to_byte_index(size_t i) {
    return i * _element_size;
  }
};

class G1WrittenCardQueueSet {
  friend class G1WrittenCardQueue;

  BufferNode::Allocator* _allocator;
  volatile bool _mutator_should_mark_cards_dirty; // No padding - rarely written.
  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, sizeof(BufferNode::Allocator*));
  volatile size_t _num_cards;
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, sizeof(size_t));
  BufferNode::Stack _buffer_list;
  DEFINE_PAD_MINUS_SIZE(2, DEFAULT_CACHE_LINE_SIZE, sizeof(BufferNode::Stack));

  using Filter = G1WrittenCardQueue::Filter;
  static Filter filter_mechanism() { return G1WrittenCardQueue::filter_mechanism(); }

  using CardValue = G1CardTable::CardValue;

  // Written contains written locations.  Converts them into card table
  // addresses, and if clean then dirties them and adds them to dcq.
  // precondition: G1UseWrittenCardQueues
  // precondition: G1WrittenCardFilter == Filter::None
  // Returns true if the dcq was flushed because of a full buffer.
  static bool mark_cards_dirty_none_filtered(void** written,
                                             size_t size,
                                             G1DirtyCardQueue& dcq,
                                             G1ConcurrentRefineStats& stats);

  // Written contains written card table addresses.  Clean entries are dirtied
  // and added to dcq.
  // precondition: G1UseWrittenCardQueues
  // precondition: G1WrittenCardFilter == Filter::Young
  // Returns true if the dcq was flushed because of a full buffer.
  static bool mark_cards_dirty_young_filtered(void** written,
                                              size_t size,
                                              G1DirtyCardQueue& dcq,
                                              G1ConcurrentRefineStats& stats);

  // Written contains written card indices.  Converts them into card table
  // addresses, and if clean then dirties them and adds them to dcq.
  // precondition: G1UseWrittenCardQueues
  // precondition: G1WrittenCardFilter == Filter::Previous
  // Returns true if the dcq was flushed because of a full buffer.
  static bool mark_cards_dirty_previous_filtered(void** written,
                                                 size_t size,
                                                 G1DirtyCardQueue& dcq,
                                                 G1ConcurrentRefineStats& stats);

  // Enqueue into dcq the clean cards in written.
  // Helper for mark_cards_dirty_XXX_filtered.
  // Returns true if the dcq was flushed because of a full buffer.
  static bool enqueue_clean_cards(CardValue** written,
                                  size_t size,
                                  G1DirtyCardQueue& dcq,
                                  G1ConcurrentRefineStats& stats);

  // Wrapper around enqueue_clean_cards to simplify calls by
  // mark_cards_dirty_XXX_filtered.
  static bool enqueue_clean_cards_helper(void** written,
                                         size_t size,
                                         G1DirtyCardQueue& dcq,
                                         G1ConcurrentRefineStats& stats);

  void** allocate_buffer();
  void deallocate_buffer(BufferNode* node);
  size_t buffer_capacity() const {
    return _allocator->buffer_capacity();
  }

  void enqueue_completed_buffer(BufferNode* node);
  BufferNode* take_completed_buffer();

  // Helper for full buffer handling.  If the current buffer is the initial
  // buffer, then allocates a real buffer, copies contents of initial buffer
  // into the real buffer, and returns true.  Otherwise returns false.
  static bool handle_full_indirect_initial_buffer(G1WrittenCardQueue& wcq,
                                                  void** buffer);

  template<size_t size_adjust, typename Marker>
  static void handle_full_buffer_inline(Thread* t, Marker marker);

  template<size_t size_adjust, typename Marker>
  static void handle_full_buffer_indirect(Thread* t, Marker marker);

  template<size_t size_adjust, typename Marker>
  static void handle_full_buffer_deferred(Thread* t, Marker marker);

public:
  G1WrittenCardQueueSet(BufferNode::Allocator* allocator);
  ~G1WrittenCardQueueSet();

  NONCOPYABLE(G1WrittenCardQueueSet);

  size_t num_cards() const;

  void abandon_completed_buffers();

  // Take a buffer from the queue set and mark it's cards dirty.
  // Returns true if a buffer was processed, false if no buffers in the set.
  // precondition: G1DeferDirtyingWrittenCards
  bool mark_cards_dirty(G1DirtyCardQueue& dcq, G1ConcurrentRefineStats& stats);

  // Used when marking cards dirty may be deferred.
  bool mutator_should_mark_cards_dirty() const;
  void set_mutator_should_mark_cards_dirty(bool value);

  static void handle_full_buffer_inline_none(Thread* t);
  static void handle_full_buffer_inline_young(Thread* t);
  static void handle_full_buffer_inline_previous(Thread* t);
  static void handle_full_buffer_indirect_none(Thread* t);
  static void handle_full_buffer_indirect_young(Thread* t);
  static void handle_full_buffer_indirect_previous(Thread* t);
  static void handle_full_buffer_deferred_none(Thread* t);
  static void handle_full_buffer_deferred_young(Thread* t);
  static void handle_full_buffer_deferred_previous(Thread* t);
};

#endif // SHARE_GC_G1_WRITTENCARDQUEUE_HPP
