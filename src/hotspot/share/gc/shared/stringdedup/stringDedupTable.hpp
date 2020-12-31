/*
 * Copyright (c) 2014, 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLE_HPP
#define SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLE_HPP

#include "memory/allocation.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "oops/typeArrayOop.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

class OopStorage;

// Provides deduplication.  There is only one instance of this class.  This
// object keeps track of all the unique character arrays used by
// deduplicated String objects.
//
// The character arrays are in a hashtable, hashed using the characters in
// the array.  The references to the arrays by the hashtable are weak,
// allowing arrays that become unreachable to be collected and their entries
// pruned from the table.  The hashtable is dynamically resized to
// accommodate the current number of hashtable entries.  There are several
// command line options controlling the growth or shrinkage of the
// hashtable.
//
// The Table and its underlying hashtable are only accessed by the dedup
// thread.  The only place where thread safety is an issue is with the GC
// callback that reports the number of dead entries in the associated
// OopStorage.
class StringDedup::Table : public CHeapObj<mtGC> {
private:
  class Entry;
  class HTable;

  // Weak storage for the string data in the table.
  static OopStorage* _table_storage;

  // The hashtable containing the string data.
  HTable* _ht;

  Table();
  ~Table();

  NONCOPYABLE(Table);

  static void num_dead_callback(size_t num_dead);

public:
  static void initialize_storage();
  static void initialize();
  static void destroy();

  // Attempt to add an entry for a shared string.  Returns true if
  // successfully added.  string_ref is an oopstorage entry from this
  // table's storage object.  It refers to a shared string.  The storage
  // entry is used by the table entry if successfully added.  If add fails,
  // the caller is responsible for releasing the storage entry.  Not
  // thread-safe.  All calls to this function must preceed any calls to
  // deduplicate().
  bool add_shared_string(oop* string_ref, Stat* stat);

  // Deduplicate java_string.  If the table already contains the string's
  // data array, replace the string's data array with the one in the table.
  // Otherwise, add the string's data array to the table.  Not thread-safe.
  // Calls to this function must follow the last call to add_shared_string().
  void deduplicate(oop java_string, Stat* stat);

  // The weak oopstorage used to record string data in the table.
  OopStorage* storage() const;

  // Rebuilding (resizing) the table.
  // First call rebuild_start.  If a state is returned, repeatedly call
  // rebuild_step until it returns false; this allows the caller to do
  // things like checking for safepoints frequently.  Call rebuild_end once
  // rebuild_step returns false to tidy up.  Also call rebuild_end if the
  // iteration is to be abandoned; this may discard some table entries, but
  // is only expected to be needed when shutting down.  Rebuild also
  // implicitly cleans the table.
  class RebuildState;

  // If rebuild is needed, returns a new rebuild state.  Otherwise
  // returns a nullptr.  The caller owns the state and must eventually
  // call rebuild_end to delete it.
  RebuildState* rebuild_start(Stat* stat);

  // Perform some rebuild work.  Returns true if any progress was
  // made, false if there is no further work associated with state.
  bool rebuild_step(RebuildState* state);

  // Record the rebuild associated with state complete and delete state.
  void rebuild_end(RebuildState* state);

  // Cleaning the table, i.e. removing cleared entries.
  // First call cleanup_start.  If a state is returned, repeatedly call
  // cleanup_step until it returns false; this allows the caller to do
  // things like checking for safepoints frequently.  Call cleanup_end once
  // cleanup_step returns false to tidy up.  Also call cleanup_end if the
  // iteration is to be abandoned.
  class CleanupState;

  // If cleanup is needed, returns a new cleanup state.  Otherwise
  // returns nullptr.  The caller owns the state and must eventually
  // call cleanup_end to delete it.
  CleanupState* cleanup_start(Stat* stat);

  // Perform some cleanup work.  Returns true if any progress was
  // made, false if there is no further work associated with state.
  bool cleanup_step(CleanupState* state);

  // Record the cleanup associated with state complete and delete state.
  void cleanup_end(CleanupState* state);

  void verify() const;
  void log_statistics() const;
};

#endif // SHARE_GC_SHARED_STRINGDEDUP_STRINGDEDUPTABLE_HPP
