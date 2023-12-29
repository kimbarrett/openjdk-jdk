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

#ifndef SHARE_UTILITIES_INTRUSIVELIST_HPP
#define SHARE_UTILITIES_INTRUSIVELIST_HPP

#include "memory/allStatic.hpp"
#include "metaprogramming/enableIf.hpp"
#include "metaprogramming/logical.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/macros.hpp"
#include <limits>
#include <type_traits>

class IntrusiveListEntry;
class IntrusiveListImpl;

/**
 * The IntrusiveList class template provides a doubly-linked list in
 * which the links between elements are embedded directly into objects
 * contained in the list.  As a result, there are no copies involved
 * when inserting objects into the list or referencing list objects,
 * and removing an object from a list need not involve destroying the
 * object.
 *
 * To be used in a IntrusiveList, an object must have a
 * IntrusiveListEntry member.  An IntrusiveList is associated with the
 * class of its elements and the entry member.
 *
 * An object can be in multiple lists at the same time, so long as
 * each list uses a different entry member.  That is, the class of the
 * object must have multiple IntrusiveListEntry members, one for each
 * list the object is simultaneously an element.
 *
 * The API for IntrusiveList is modelled on the containers provided by
 * the C++ standard library.  In particular, iteration over the
 * elements is provided via iterator classes.
 *
 * IntrusiveLists support polymorphic elements.  Because the objects
 * in a list are externally managed, rather than being embedded values
 * in the list, the actual type of such objects may be more specific
 * than the list's element type.
 *
 * * T is the class of the elements in the list.  Must be a class type,
 * possibly const-qualified.
 *
 * * has_size determines whether the list has a size() operation, returning
 * the number of elements in the list.  If the operation is requested, it has
 * constant-time complexity.  The default is to not provide a constant-time
 * size() operation.
 *
 * * entry_key designates the IntrusiveListEntry subobject of T associated
 * with this list.  See IntrusiveListAccess.
 *
 * A const-element iterator has a const-qualified element type.  Such an
 * iterator provides const-qualified access to the elements it designates.
 * A list's const_iterator type is always a const-element iterator type.
 *
 * A const-element list has a const-qualified element type.  Such a list
 * provides const-element iterators and const-qualified access to its
 * elements.  A const list similarly provides const access to elements, but
 * does not support changing the sequence of elements.
 *
 * A const object can only be added to a const-element list.  Adding a const
 * object to a non-const-element list would be an implicit casting away of the
 * object's const qualifier.
 *
 * Some operations that remove elements from a list take a disposer argument.
 * This is a function object that will be called with one argument, of type
 * pointer to T, to a removed element.  This function should "dispose" of the
 * argument object when called, such as by deleting it.  The result of the
 * call is ignored.
 *
 * Usage of IntrusiveList involves defining an element class which contains a
 * IntrusiveListEntry member, specializing IntrusiveListAccess::get_entry for
 * that class, and using a corresponding specialization of the IntrusiveList
 * class.  For the simplest case of a class with only one entry subobject:
 *
 * class MyClass {
 *   ...
 *   IntrusiveListEntry _entry;
 *   // Friendship is not needed if _entry is public.
 *   friend class IntrusiveListAccess<MyClass>;
 *   ...
 * public:
 *   ...
 * };
 *
 * // Get the entry for IntrusiveListEntry::DefaultKey.
 * template<>
 * const IntrusiveListEntry&
 * IntrusiveListAccess<MyClass>::get_entry(const MyClass& v) {
 *   return v._entry;
 * }
 *
 * Then declare a list of MyClass objects without size tracking:
 *   IntrusiveList<MyClass> mylist;
 *
 * If MyClass has multiple entries, then we need a different key for each.
 *
 * class MyClass {
 *   ...
 *   IntrusiveListEntry _entry0;
 *   IntrusiveListEntry _entry1;
 *   // Friendship is not needed if _entry is public.
 *   friend class IntrusiveListAccess<MyClass>;
 *   ...
 * public:
 *   using EntryKey = IntrusiveListEntry::Key;
 *   static const EntryKey entry0 = static_cast<EntryKey>(0);
 *   static const EntryKey entry1 = static_cast<EntryKey>(1);
 *   ...
 * };
 *
 * and a specialized definition of the two argument get_entry is needed:
 *
 * template<>
 * const IntrusiveListEntry&
 * IntrusiveListAccess<MyClass>::get_entry(const MyClass& v,
 *                                         IntrusiveListEntry::Key key) {
 *   switch (key) {
 *   case MyClass::entry0: return v._entry0;
 *   case MyClass::entry1: return v._entry1;
 *   default: ShouldNotReachHere();
 *   }
 * }
 *
 * and declare a list for each entry:
 *   IntrusiveList<MyClass, false, MyClass::entry0> list0;  // no size tracking
 *   IntrusiveList<MyClass, true , MyClass::entry1> list1;  // has size tracking
 */

/**
 * A class with an IntrusiveListEntry member can be used as an element
 * of a corresponding specialization of IntrusiveList.  A class can have
 * multiple IntrusiveListEntry members, which are designated by Key values.
 */
class IntrusiveListEntry {
  friend class IntrusiveListImpl;

public:
  /** Make an entry not attached to any list. */
  IntrusiveListEntry()
    : _prev(nullptr),
      _next(nullptr)
      DEBUG_ONLY(COMMA _list(nullptr))
   {}

  /**
   * Destroy the entry.
   *
   * precondition: not an element of a list.
   */
  ~IntrusiveListEntry() NOT_DEBUG(= default);

  NONCOPYABLE(IntrusiveListEntry);

  /** Test whether this entry is attached to some list. */
  bool is_attached() const {
    bool result = (_prev != nullptr);
    assert(result == (_next != nullptr), "inconsistent entry");
    return result;
  }

  /** Designator for an entry subobject of an object. */
  enum class Key {};

  /** The default designator.  */
  static const Key DefaultKey = static_cast<Key>(0);

private:
  // _prev and _next are the links between elements / root entries in
  // an associated list.  The values of these members are type-erased
  // void*.  The IntrusiveListImpl::IOps class is used to encode,
  // decode, and manipulate the type-erased values.
  //
  // Members are mutable and we deal exclusively with pointers to
  // const to make const_references and const_iterators easier to use;
  // an object being const doesn't prevent modifying its list state.
  mutable const void* _prev;
  mutable const void* _next;
  // The list containing this entry, if any.
  // Debug-only, for use in validity checks.
  DEBUG_ONLY(mutable IntrusiveListImpl* _list;)
};

// IntrusiveListAccess provides the list implementation with the mapping from
// an element class object to an entry subobject, with the subobject
// designated by a Key value.
//
// The key provides a level of indirection between a list declaration and the
// access code.  A key can be used in a list declaration before the class is
// complete.  The class only needs to be complete where an access occurs,
// e.g. where operations on the list are performed.  It's also designed to
// minimize the effort (amount of source code) required of the element class -
// just define one fully specialized function.
//
// Everything in this class is private, with friend access given to the list
// implementation.  That prevents this class from making the entry subobjects
// of a class accessible to others.  Typically the entry subobjects are
// private in the element class, with friendship access granted to the
// associated specialization of this class.
//
// Having two get_entry functions with different signatures simplifies uses in
// the very common case where an element class has only one entry subobject.
// In that case, all that's needed is a specialized definition of the
// one-argument get_entry function.  If there are multiple entry subobjects
// then only the two-argument specialized get_entry definition is needed.
//
// In the case of multiple subobjects with associated keys, we could pass the
// key as a template argument rather than an ordinary argument.  That would
// require a more syntactically complicated function specialization, but might
// allow better compile-time error checking.
//
// Other mechanisms for providing that mapping include:
//
// - Provide the list with a pointer-to-data-member for the entry subobject.
// This is in many cases the simplest, from the POV of what is required by the
// application.  However, it requires the element class be complete anywhere a
// list is declared.  It also tends to run afoul of odd corner case compiler
// bugs.
//
// - Provide the list with a function pointer for accessing the entry
// subobject.  That typically requires at least as much list user setup as
// required by the current mechanism.  It may also expose (via that function)
// the entry subobject to other code, though that's less important in a closed
// "library" like HotSpot.  It may also require the element class to be
// complete where the list is declared, or instead involve some code
// contortions to avoid that.

/**
 * This class provides IntrusiveList with access to IntrusiveListEntry
 * subobjects in an object of a class.  The class must have an associated
 * specialized definition for one of the get_entry functions declared here.
 *
 * If a class C has one IntrusiveListEntry, designated by DefaultKey, then it
 * should have an associated definition for
 *   const IntrusiveListEntry& IntrusiveListAccess<C>::get_entry(const C&)
 * that returns a reference to the entry in the argument object.
 *
 * If a class C has multiple IntrusiveListEntry's, then it should have an
 * associated definition for
 *   const IntrusiveListEntry& IntrusiveListAccess<C>::get_entry(const C&, IntrusiveListEntry::Key)
 * returning a reference to the entry in the argument object designated by the
 * key.  Note that the key argument is always a constant, so inlining is
 * likely profitable.
 *
 * Only one of the specializatiions should be defined.  In either case, the
 * definition must return a reference to the entry subobject of the argument
 * object for the key.
 */
template<typename T>
class IntrusiveListAccess {
  friend class IntrusiveListImpl;

  // Don't put any other declarations here, such as convenient type aliases.
  // Any such are visible in the specialized get_entry definitions provided by
  // element classes, possibly unexpectedly shadowing identifiers referenced
  // by those definitions.

  // This function gets called when the key is DefaultKey and there isn't a
  // specialized definition.  It just forwards to the corresponding two
  // argument function.  This allows a class having multiple entries to only
  // specialize the two-argument function.
  static const IntrusiveListEntry&
  get_entry(std::add_const_t<T>& v) {
    return get_entry(v, IntrusiveListEntry::DefaultKey);
  }

  // If there is a two-argument call, there must be a specialized definition.
  // Two-argument calls are made for non-default keys and by the unspecialized
  // one-argument overload above.
  static const IntrusiveListEntry&
  get_entry(std::add_const_t<T>& v, IntrusiveListEntry::Key entry_key) = delete;
};

template<typename T,
         bool has_size = false,
         IntrusiveListEntry::Key key = IntrusiveListEntry::DefaultKey>
class IntrusiveList;

// IntrusiveListImpl provides implementation support for IntrusiveList.
// There's nothing for clients to see here. That support is all private, with
// the IntrusiveList class given access via friendship.
class IntrusiveListImpl {
public:
  struct TestSupport;            // For unit tests

private:
  using Entry = IntrusiveListEntry;

  template<typename T, bool, Entry::Key>
  friend class IntrusiveList;

  using size_type = size_t;
  using difference_type = ptrdiff_t;

  Entry _root;

  IntrusiveListImpl();
  ~IntrusiveListImpl() NOT_DEBUG(= default);

  NONCOPYABLE(IntrusiveListImpl);

  // Tag manipulation for encoded void*; see IOps.
  static const uintptr_t _tag_alignment = 2;

  static bool is_tagged_root_entry(const void* ptr) {
    return !is_aligned(ptr, _tag_alignment);
  }

  static const void* add_tag_to_root_entry(const Entry* entry) {
    assert(is_aligned(entry, _tag_alignment), "must be");
    const void* untagged = entry;
    return static_cast<const char*>(untagged) + 1;
  }

  static const Entry* remove_tag_from_root_entry(const void* ptr) {
    assert(is_tagged_root_entry(ptr), "precondition");
    const void* untagged = static_cast<const char*>(ptr) - 1;
    assert(is_aligned(untagged, _tag_alignment), "must be");
    return static_cast<const Entry*>(untagged);
  }

  const Entry* root_entry() const { return &_root; }

  static void detach(const Entry& entry) {
    entry._prev = nullptr;
    entry._next = nullptr;
    DEBUG_ONLY(entry._list = nullptr;)
  }

  // Support for optional constant-time size() operation.
  template<bool has_size> class SizeBase;

  // Relevant type aliases.  A corresponding specialization is used directly
  // by IntrusiveList, and by the list's iterators to obtain their
  // corresponding nested types.
  template<typename T>
  struct ListTraits : public AllStatic {
    static_assert(std::is_class<T>::value, "precondition");
    // May be const, but not volatile.
    static_assert(!std::is_volatile<T>::value, "precondition");

    using size_type = IntrusiveListImpl::size_type;
    using difference_type = IntrusiveListImpl::difference_type;
    using value_type = T;
    using pointer = std::add_pointer_t<value_type>;
    using const_pointer = std::add_pointer_t<std::add_const_t<value_type>>;
    using reference = std::add_lvalue_reference_t<value_type>;
    using const_reference = std::add_lvalue_reference_t<std::add_const_t<value_type>>;
  };

  // Like std::distance, but returns size_type because that's what's needed here.
  template<typename Iterator>
  static size_type distance(Iterator from, Iterator to) {
    size_type result = 0;
    for ( ; from != to; ++result, ++from) {}
    return result;
  }

  // Iterator support.  IntrusiveList defines its iterator types as
  // specializations of this class.
  template<typename T,
           IntrusiveListEntry::Key entry_key,
           bool is_forward>
  class IteratorImpl;

  // Iterator support.  Provides (static) functions for manipulating
  // iterators.  These are used to implement iterators and list
  // operations related to iterators, but are not part of the public
  // API for iterators.
  template<typename Iterator> class IOps;

  // Select which get_entry overload to call, based on the key value, using
  // SFINAE to prevent introducing a call site for the other.
  // C++17 if-constexpr simplification candidate.
  template<Entry::Key entry_key, typename T,
           ENABLE_IF(entry_key == Entry::DefaultKey)>
  static const Entry& get_entry(const T& v) {
    return IntrusiveListAccess<T>::get_entry(v);
  }

  template<Entry::Key entry_key, typename T,
           ENABLE_IF(entry_key != Entry::DefaultKey)>
  static const Entry& get_entry(const T& v) {
    return IntrusiveListAccess<T>::get_entry(v, entry_key);
  }

  // Predicate metafunction for determining whether T is a non-const
  // IntrusiveList type.
  template<typename T>
  struct IsListType : public std::false_type {};

#ifdef ASSERT
  // Get entry's containing list; null if entry not in a list.
  static const IntrusiveListImpl* entry_list(const Entry& entry);
  // Set entry's containing list; list may be null.
  static void set_entry_list(const Entry& entry, IntrusiveListImpl* list);
#endif // ASSERT
};

// Base class for IntrusiveList, with specializations either providing
// or not providing constant-time size.

template<bool has_size>
class IntrusiveListImpl::SizeBase {
protected:
  SizeBase() = default;
  ~SizeBase() = default;

  void increase_size(size_type n) {}
  void decrease_size(size_type n) {}
};

template<>
class IntrusiveListImpl::SizeBase<true> {
public:
  size_type size() const { return _size; }

protected:
  SizeBase() : _size(0) {}
  ~SizeBase() = default;

  void increase_size(size_type n) {
    assert((std::numeric_limits<size_type>::max() - n) >= _size, "size overflow");
    _size += n;
  }

  void decrease_size(size_type n) {
    assert(n <= _size, "size underflow");
    _size -= n;
  }

private:
  size_type _size;
};

template<typename T, bool has_size, IntrusiveListEntry::Key key>
struct IntrusiveListImpl::IsListType<IntrusiveList<T, has_size, key>>
  : public std::true_type
{};

// The IOps class (short for IteratorOperations) provides operations for
// encoding, decoding, and manipulating type-erased void* values representing
// objects in a list.  The encoded void* provides a discriminated union of the
// following:
//
// - T*: a pointer to a list element.
// - IntrusiveListEntry*: a pointer to a list's root entry.
// - nullptr: a pointer to no object.
//
// IntrusiveListEntry uses such encoded values to refer to the next or
// previous object in a list, e.g. to represent the links between
// objects.
//
// IteratorImpl uses such encoded values to refer to the object that
// represents the iterator.  A singular iterator is represented by an
// encoded null.  A dereferenceable iterator is represented by an
// encoded pointer to a list element.  An encoded list root entry is
// used to represent either an end-of-list or before-the-beginning
// iterator, depending on context.
//
// The encoding of these values uses a tagged void pointer scheme.
// null represents itself.  A list element (T*) is distinguished from
// a IntrusiveListEntry* via the low address bit.  If the low bit is
// set, the value is a IntrusiveListEntry*; specifically, it is one
// byte past the pointer to the entry.  Otherwise, it is a list
// element.  [This requires all value types and IntrusiveListEntry to
// have an alignment of at least 2.]
//
// This encoding leads to minimal cost for typical correct iteration patterns.
// Dereferencing an iterator referring to a list element consists of just
// reinterpreting the type of the iterator's internal value.  And for
// iteration over a range denoted by a pair of iterators, until the iteration
// reaches the end of the range the current iterator always refers to a list
// element.  Similarly, incrementing an iterator consists of just a load from
// the iterator's internal value plus a constant offset.
//
// IOps also provides a suite of operations for manipulating iterators and
// list elements, making use of that encoding.  This allows the implementation
// of iterators and lists to be written in terms of these higher level
// operations, without needing to deal with the underlying encoding directly.
//
// Note that various functions provided by this class take a const_reference
// argument.  This means some of these functions may break the rule against
// putting const elements in lists with non-const elements.  It is up to
// callers to ensure that doesn't really happen and result in implicitly
// casting away const of the passed argument.  That is, if the list has
// non-const elements then the actual argument must be non-const, even though
// the function parameter is const_reference.  We do it this way because
// having the overloads for both, with one being conditional, would
// significantly expand the code surface and complexity here.  Instead we
// expect the list API to enforce the invariant, which has the added benefit
// of having improper usage fail to compile at that level rather than deep in
// the implementation.  See splice() for example.
template<typename Iterator>
class IntrusiveListImpl::IOps : AllStatic {
  using Impl = IntrusiveListImpl;
  using ListTraits = typename Iterator::ListTraits;
  using const_reference = typename ListTraits::const_reference;

  static const bool _is_forward = Iterator::_is_forward;

  static const void* make_encoded_value(const_reference value) {
    return &value;
  }

  static const void* make_encoded_value(const Entry* entry) {
    return add_tag_to_root_entry(entry);
  }

  static const Entry& resolve_to_entry(Iterator i) {
    assert_not_singular(i);
    const void* encoded = encoded_value(i);
    if (is_tagged_root_entry(encoded)) {
      return *(remove_tag_from_root_entry(encoded));
    } else {
      return get_entry(dereference_element_ptr(encoded));
    }
  }

  // Get the list element from an encoded pointer to list element.
  static const_reference dereference_element_ptr(const void* encoded_ptr) {
    return *static_cast<typename ListTraits::const_pointer>(encoded_ptr);
  }

  static Iterator next(Iterator i) {
    return Iterator(resolve_to_entry(i)._next);
  }

  static Iterator prev(Iterator i) {
    return Iterator(resolve_to_entry(i)._prev);
  }

  static Iterator next(const_reference value) {
    return Iterator(get_entry(value)._next);
  }

  static Iterator prev(const_reference value) {
    return Iterator(get_entry(value)._prev);
  }

  static void attach_impl(const_reference prev, Iterator next) {
    get_entry(prev)._next = encoded_value(next);
    resolve_to_entry(next)._prev = make_encoded_value(prev);
  }

  static void attach_impl(Iterator prev, const_reference next) {
    resolve_to_entry(prev)._next = make_encoded_value(next);
    get_entry(next)._prev = encoded_value(prev);
  }

  static void iter_attach_impl(Iterator prev, Iterator next) {
    resolve_to_entry(prev)._next = encoded_value(next);
    resolve_to_entry(next)._prev = encoded_value(prev);
  }

public:
  static const void* encoded_value(Iterator i) { return i._encoded_value; }

  static bool is_singular(Iterator i) {
    return encoded_value(i) == nullptr;
  }

  static bool is_root_entry(Iterator i) {
    return is_tagged_root_entry(encoded_value(i));
  }

  // Corresponding is_element is not used, so not provided.

  static const Entry& get_entry(const_reference v) {
    return Impl::get_entry<Iterator::_entry_key>(v);
  }

  static const_reference dereference(Iterator i) {
    assert_not_singular(i);
    assert(!is_root_entry(i), "dereference end-of-list iterator");
    return dereference_element_ptr(encoded_value(i));
  }

  // Get the predecessor / successor (according to the iterator's
  // direction) of the argument.  Reference arguments are preferred;
  // the iterator form should only be used when the iterator is not
  // already known to be dereferenceable.  (The iterator form of
  // successor is not provided; for an iterator to have a successor,
  // the iterator must be dereferenceable.)

  static Iterator successor(const_reference value) {
    return _is_forward ? next(value) : prev(value);
  }

  static Iterator predecessor(const_reference value) {
    return _is_forward ? prev(value) : next(value);
  }

  static Iterator iter_predecessor(Iterator i) {
    return _is_forward ? prev(i) : next(i);
  }

  // Attach pred to succ such that, after the operation,
  // predecessor(succ) == pred.  A reference argument is required when
  // it is not already in the list, since iterator_to is invalid in
  // that situation.  Reference arguments are preferred; an iterator
  // argument should only be used when it is not already known to be
  // dereferenceable.  That is, the first argument should only be an
  // iterator if it might be a before-the-beginning (pseudo)iterator.
  // Similarly, the second argument should only be an iterator if it
  // might be an end-of-list iterator.  (The two-reference case is not
  // provided because that form is never needed.)

  // Mixed reference / iterator attachment.
  template<typename PredType, typename SuccType>
  static void attach(const PredType& pred, const SuccType& succ) {
    _is_forward ? attach_impl(pred, succ) : attach_impl(succ, pred);
  }

  // Iterator to iterator attachment.
  static void iter_attach(Iterator pred, Iterator succ) {
    _is_forward ? iter_attach_impl(pred, succ) : iter_attach_impl(succ, pred);
  }

  template<typename Iterator2>
  static Iterator make_iterator(Iterator2 i) {
    return Iterator(IOps<Iterator2>::encoded_value(i));
  }

  static Iterator make_iterator_to(const_reference value) {
    return Iterator(make_encoded_value(value));
  }

  static Iterator make_begin_iterator(const Impl& impl) {
    const Entry* entry = impl.root_entry();
    return Iterator(_is_forward ? entry->_next : entry->_prev);
  }

  static Iterator make_end_iterator(const Impl& impl) {
    return Iterator(make_encoded_value(impl.root_entry()));
  }

  static void assert_not_singular(Iterator i) {
    assert(!is_singular(i), "singular iterator");
  }

  static void assert_is_in_some_list(Iterator i) {
    assert_not_singular(i);
    assert(list_ptr(i) != nullptr,
           "Invalid iterator " PTR_FORMAT, p2i(encoded_value(i)));
  }

#ifdef ASSERT

  static const Impl* list_ptr(Iterator i) {
    return entry_list(resolve_to_entry(i));
  }

#endif // ASSERT
};

/**
 * Bi-directional constant (e.g. not output) iterator for iterating
 * over the elements of an IntrusiveList.  The IntrusiveList class
 * uses specializations of this class as its iterator types.
 *
 * An iterator may be either const-element or non-const-element.  The value
 * type of a const-element iterator is const-qualified, and a const-element
 * iterator only provides access to const-qualified elements.  Similarly, a
 * non-const-element iterator provides access to unqualified elements.  A
 * non-const-element iterator can be converted to a const-element iterator,
 * but not vice versa.
 */
template<typename T,
         IntrusiveListEntry::Key entry_key,
         bool is_forward>
class IntrusiveListImpl::IteratorImpl {
  friend class IntrusiveListImpl;

  static const bool _is_forward = is_forward;
  static const bool _is_const_element = std::is_const<T>::value;
  static const IntrusiveListEntry::Key _entry_key = entry_key;

  using Impl = IntrusiveListImpl;
  using ListTraits = Impl::ListTraits<T>;
  using IOps = Impl::IOps<IteratorImpl>;

  // Test whether From is an iterator type different from this type that can
  // be implicitly converted to this iterator type.  A const_element iterator
  // type supports implicit conversion from the corresponding
  // non-const-element iterator type.
  template<typename From>
  static constexpr bool is_convertible_iterator() {
    using NonConst = IteratorImpl<std::remove_const_t<T>, entry_key, _is_forward>;
    return _is_const_element && std::is_same<From, NonConst>::value;
  }

public:
  /** Type of an iterator's value. */
  using value_type = typename ListTraits::value_type;

  /** Type of a reference to an iterator's value. */
  using reference = typename ListTraits::reference;

  /** Type of a pointer to an iterator's value. */
  using pointer = typename ListTraits::pointer;

  /** Type for distance between iterators. */
  using difference_type = typename ListTraits::difference_type;

  // TODO: We don't have access to <iterator>, so we can't provide the
  // iterator_category type.  Maybe someday...
  // using iterator_category = std::bidirectional_iterator_tag;

  /** Construct a singular iterator. */
  IteratorImpl() : _encoded_value(nullptr) {}

  ~IteratorImpl() = default;
  IteratorImpl(const IteratorImpl&) = default;
  IteratorImpl& operator=(const IteratorImpl&) = default;

  /** Implicit conversion from non-const to const element type. */
  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  IteratorImpl(const From& other)
    : _encoded_value(Impl::IOps<From>::encoded_value(other))
  {}

  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  IteratorImpl& operator=(const From& other) {
    return *this = IteratorImpl(other);
  }

  /**
   * Return a reference to the iterator's value.
   *
   * precondition: this is dereferenceable.
   * complexity: constant.
   */
  reference operator*() const {
    return const_cast<reference>(IOps::dereference(*this));
  }

  /**
   * Return a pointer to the iterator's value.
   *
   * precondition: this is dereferenceable.
   * complexity: constant.
   */
  pointer operator->() const {
    return &this->operator*();
  }

  /**
   * Change this iterator to refer to the successor element (per the
   * iterator's direction) in the list, or to the end of the list.
   * Return a reference to this iterator.
   *
   * precondition: this is dereferenceable.
   * postcondition: this is dereferenceable or end-of-list.
   * complexity: constant.
   */
  IteratorImpl& operator++() {
    IOps::assert_is_in_some_list(*this);
    *this = IOps::successor(this->operator*());
    return *this;
  }

  /**
   * Make a copy of this iterator, then change this iterator to refer
   * to the successor element (per the iterator's direction) in the
   * list, or to the end of the list.  Return the copy.
   *
   * precondition: this is dereferenceable.
   * postcondition: this is dereferenceable or end-of-list.
   * complexity: constant.
   */
  IteratorImpl operator++(int) {
    IteratorImpl result = *this;
    this->operator++();
    return result;
  }

  /**
   * Change this iterator to refer to the preceeding element (per the
   * iterator's direction) in the list.  Return a reference to this
   * iterator.
   *
   * precondition: There exists an iterator i such that ++i equals this.
   * postcondition: this is dereferenceable.
   * complexity: constant.
   */
  IteratorImpl& operator--() {
    IOps::assert_is_in_some_list(*this);
    *this = IOps::iter_predecessor(*this);
    // Must not have been (r)begin iterator.
    assert(!IOps::is_root_entry(*this), "iterator decrement underflow");
    return *this;
  }

  /**
   * Make a copy of this iterator, then change this iterator to refer
   * to the preceeding element (per the iterator's direction) in the
   * list.  Return the copy.
   *
   * precondition: There exists an iterator i such that ++i equals this.
   * postcondition: this is dereferenceable.
   * complexity: constant.
   */
  IteratorImpl operator--(int) {
    IteratorImpl result = *this;
    this->operator--();
    return result;
  }

  /**
   * Return true if this and other refer to the same element of a list, or
   * refer to end-of-list for the same list, or are both singular.
   *
   * precondition: this and other are both valid iterators for the same list,
   * or both are singular.
   * complexity: constant.
   */
  bool operator==(const IteratorImpl& other) const {
    // C++14 24.2.5/2: The domain of == for iterators is that of iterators
    // over the same underlying sequence.  However, C++14 additionally permits
    // comparison of value-initialized iterators, which compare equal to other
    // value-initialized iterators of the same type.  We can't distinguish
    // between a value-initialized iterator and a singular iterator.  So
    // singular iterators with the same (ignoring const qualification) element
    // type are considered equal.
#ifdef ASSERT
    if (IOps::is_singular(*this)) {
      assert(IOps::is_singular(other), "Comparing singular and non-singular");
    } else {
      assert(!IOps::is_singular(other), "Comparing singular and non-singular");
      IOps::assert_is_in_some_list(*this);
      IOps::assert_is_in_some_list(other);
      assert(IOps::list_ptr(*this) == IOps::list_ptr(other),
             "Comparing iterators from different lists");
    }
#endif // ASSERT
    return IOps::encoded_value(*this) == IOps::encoded_value(other);
  }

  /**
   * Return true if this and other are not ==.
   *
   * precondition: this and other are both valid iterators for the same list,
   * or both are singular.
   * complexity: constant.
   */
  bool operator!=(const IteratorImpl& other) const {
    return !(*this == other);
  }

  // Add ConvertibleFrom OP IteratorImpl overloads, because these are
  // not handled by the corresponding member function plus implicit
  // conversions.  For example, const_iterator == iterator is handled
  // by const_iterator::operator==(const_iterator) plus implicit
  // conversion of iterator to const_iterator.  But we need an
  // additional overload to handle iterator == const_iterator when those
  // types are different.

  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  friend bool operator==(const From& lhs, const IteratorImpl& rhs) {
    return rhs == lhs;
  }

  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  friend bool operator!=(const From& lhs, const IteratorImpl& rhs) {
    return rhs != lhs;
  }

private:
  // An iterator refers to either an object in the list, the root
  // entry of the list, or null if singular.  See IOps
  // for details of the encoding.
  const void* _encoded_value;

  // Allow explicit construction from an encoded const void*
  // value.  But require exactly that type, disallowing any implicit
  // conversions.  Without that restriction, certain kinds of usage
  // errors become both more likely and harder to diagnose the
  // resulting compilation errors.  [The remaining diagnostic
  // difficulties could be eliminated by making EncodedValue a non-public
  // class for carrying the encoded void* to iterator construction.]
  template<typename EncodedValue,
           ENABLE_IF(std::is_same<EncodedValue, const void*>::value)>
  explicit IteratorImpl(EncodedValue encoded_value)
    : _encoded_value(encoded_value)
  {}
};

template<typename T, bool has_size, IntrusiveListEntry::Key entry_key>
class IntrusiveList : public IntrusiveListImpl::SizeBase<has_size> {
  // Give access to other instantiations, for splice().
  template<typename U, bool, IntrusiveListEntry::Key>
  friend class IntrusiveList;

  // Give access for unit testing.
  friend struct IntrusiveListImpl::TestSupport;

  using Entry = IntrusiveListEntry;
  using Impl = IntrusiveListImpl;
  using ListTraits = Impl::ListTraits<T>;

  // A subsequence of one list can be transferred to another list via splice
  // if the lists have the same (ignoring const qualifiers) element type, use
  // the same entry member, and either the receiver is a const-element list
  // or neither is a const-element list.  A const element of a list cannot be
  // transferred to a list with non-const elements.  That would effectively be
  // a quiet casting away of const.  Assuming Other is a List, these
  // constraints are equivalent to the constraints on conversion of
  // Other::iterator -> iterator.  The presence or absence of constant-time
  // size support for either of the lists doesn't affect whether splicing is
  // permitted.
  template<typename Other>
  static constexpr bool can_splice_from() {
    return Conjunction<Impl::IsListType<Other>,
                       HasConvertibleIterator<Other, iterator>>::value;
  }

  // Helper for can_splice_from, delaying instantiation that includes
  // "Other::iterator" until Other is known to be a List type.
  // C++17 if-constexpr cleanup candidate.
  template<typename Other, typename Iterator>
  struct HasConvertibleIterator
    : public BoolConstant<std::is_convertible<typename Other::iterator, Iterator>::value>
  {};

  // Swapping can be thought of as bi-directional slicing (see
  // can_splice_from).  So Other::iterator must be the same as iterator.
  template<typename Other>
  static constexpr bool can_swap() {
    return Conjunction<Impl::IsListType<Other>,
                       HasSameIterator<Other, iterator>>::value;
  }

  // Helper for can_swap, delaying instantiation that includes
  // "Other::iterator" until Other is known to be a List type.
  // C++17 if-constexpr cleanup candidate.
  template<typename Other, typename Iterator>
  struct HasSameIterator
    : public BoolConstant<std::is_same<typename Other::iterator, Iterator>::value>
  {};

public:
  /** Flag indicating presence of a constant-time size() operation. */
  static const bool _has_size = has_size;

  /** Type of the size of the list. */
  using size_type = typename ListTraits::size_type;

  /** The difference type for iterators. */
  using difference_type = typename ListTraits::difference_type;

  /** Type of list elements. */
  using value_type = typename ListTraits::value_type;

  /** Type of a pointer to a list element. */
  using pointer = typename ListTraits::pointer;

  /** Type of a pointer to a const list element. */
  using const_pointer = typename ListTraits::const_pointer;

  /** Type of a reference to a list element. */
  using reference = typename ListTraits::reference;

  /** Type of a reference to a const list element. */
  using const_reference = typename ListTraits::const_reference;

  /** Forward iterator type. */
  using iterator =
    Impl::IteratorImpl<T, entry_key, true>;

  /** Forward iterator type with const elements. */
  using const_iterator =
    Impl::IteratorImpl<std::add_const_t<T>, entry_key, true>;

  /** Reverse iterator type. */
  using reverse_iterator =
    Impl::IteratorImpl<T, entry_key, false>;

  /** Reverse iterator type with const elements. */
  using const_reverse_iterator =
    Impl::IteratorImpl<std::add_const_t<T>, entry_key, false>;

  /** Make an empty list. */
  IntrusiveList() : _impl() {}

  /**
   * Destroy the list.
   *
   * precondition: empty()
   */
  ~IntrusiveList() = default;

  NONCOPYABLE(IntrusiveList);

  /**
   * Inserts value at the front of the list.  Does not affect the
   * validity of iterators or element references for this list.
   *
   * precondition: value must not already be in a list using the same entry.
   * complexity: constant.
   */
  void push_front(reference value) {
    insert(begin(), value);
  }

  /**
   * Inserts value at the back of the list.  Does not affect the
   * validity of iterators or element references for this list.
   *
   * precondition: value must not already be in a list using the same entry.
   * complexity: constant.
   */
  void push_back(reference value) {
    insert(end(), value);
  }

  /**
   * Removes the front element from the list, and applies the
   * disposer, if any, to the removed element.  The list may not be in
   * a consistent state when the disposer is called.  Invalidates
   * iterators for the removed element.
   *
   * precondition: !empty()
   * complexity: constant.
   */
  void pop_front() {
    pop_front_and_dispose(NopDisposer());
  }

  template<typename Disposer>
  void pop_front_and_dispose(Disposer disposer) {
    erase_and_dispose(begin(), disposer);
  }

  /**
   * Removes the back element from the list, and applies the disposer,
   * if any, to the removed element.  The list may not be in a
   * consistent state when the disposer is called.  Invalidates
   * iterators for the removed element.
   *
   * precondition: !empty()
   * complexity: constant.
   */
  void pop_back() {
    pop_back_and_dispose(NopDisposer());
  }

  template<typename Disposer>
  void pop_back_and_dispose(Disposer disposer) {
    erase_and_dispose(rbegin(), disposer);
  }

  /**
   * Returns a [const_]reference to the front element of the list.
   *
   * precondition: !empty()
   * complexity: constant.
   */
  reference front() { return *begin(); }
  const_reference front() const { return *begin(); }

  /**
   * Returns a [const_]reference to the back element of the list.
   *
   * precondition: !empty()
   * complexity: constant.
   */
  reference back() { return *rbegin(); }
  const_reference back() const { return *rbegin(); }

  /**
   * Returns a [const_]iterator referring to the first element of the
   * list, or end-of-list if the list is empty.
   *
   * complexity: constant.
   */
  iterator begin() {
    return Impl::IOps<iterator>::make_begin_iterator(_impl);
  }

  const_iterator begin() const {
    return cbegin();
  }

  const_iterator cbegin() const {
    return Impl::IOps<const_iterator>::make_begin_iterator(_impl);
  }

  /**
   * Returns a [const_]iterator referring to the end-of-list.
   *
   * complexity: constant.
   */
  iterator end() {
    return Impl::IOps<iterator>::make_end_iterator(_impl);
  }

  const_iterator end() const {
    return cend();
  }

  const_iterator cend() const {
    return Impl::IOps<const_iterator>::make_end_iterator(_impl);
  }

  /**
   * Returns a [const_]reverse_iterator referring to the last element
   * of the list, or end-of-reversed-list if the list is empty.
   *
   * complexity: constant.
   */
  reverse_iterator rbegin() {
    return Impl::IOps<reverse_iterator>::make_begin_iterator(_impl);
  }

  const_reverse_iterator rbegin() const {
    return crbegin();
  }

  const_reverse_iterator crbegin() const {
    return Impl::IOps<const_reverse_iterator>::make_begin_iterator(_impl);
  }

  /**
   * Returns a [const_]reverse_iterator referring to the
   * end-of-reversed-list.
   *
   * complexity: constant.
   */
  reverse_iterator rend() {
    return Impl::IOps<reverse_iterator>::make_end_iterator(_impl);
  }

  const_reverse_iterator rend() const {
    return crend();
  }

  const_reverse_iterator crend() const {
    return Impl::IOps<const_reverse_iterator>::make_end_iterator(_impl);
  }

  /**
   * Returns true if list contains no elements.
   *
   * complexity: constant.
   */
  bool empty() const {
    return cbegin() == cend();
  }

  /**
   * Returns the number of elements in the list.
   *
   * complexity: O(length())
   */
  size_type length() const {
    return Impl::distance(cbegin(), cend());
  }

  /**
   * Removes the element referred to by i from the list, then applies
   * the disposer, if any, to the removed element.  The list may not
   * be in a consistent state when the disposer is called.  Returns an
   * iterator for the successor of i.  Invalidates iterators referring
   * to the removed element.
   *
   * precondition: i must be a dereferenceable iterator for the list.
   * complexity: constant.
   */
  iterator erase(const_iterator i) {
    return erase_and_dispose(i, NopDisposer());
  }

  reverse_iterator erase(const_reverse_iterator i) {
    return erase_and_dispose(i, NopDisposer());
  }

  template<typename Disposer>
  iterator erase_and_dispose(const_iterator i, Disposer disposer) {
    return erase_one_and_dispose<iterator>(i, disposer);
  }

  template<typename Disposer>
  reverse_iterator erase_and_dispose(const_reverse_iterator i, Disposer disposer) {
    return erase_one_and_dispose<reverse_iterator>(i, disposer);
  }

  /**
   * Removes v from the list.  Returns an iterator for the successor
   * of v in the list.  Invalidates iterators referring to v.
   *
   * precondition: v must be in the list.
   * complexity: constant.
   */
  iterator erase(const_reference v) {
    // This may seem a little roundabout, but it gets good debug-only error
    // checking with minimal source code and no additional overhead in release.
    return erase(iterator_to(v));
  }

private:

  template<typename Result, typename Iterator, typename Disposer>
  Result erase_one_and_dispose(Iterator i, Disposer disposer) {
    using IOps = Impl::IOps<Iterator>;
    assert_is_iterator(i);
    const_reference value = *i++;
    IOps::iter_attach(IOps::predecessor(value), i);
    detach(value);
    disposer(disposer_arg(value));
    return make_iterator<Result>(i);
  }

public:

  /**
   * Removes the elements in the range designated by from and to.
   * Applies the disposer, if any, to each removed element.  The list
   * may not be in a consistent state when the disposer is called.
   * Returns an iterator referring to the end of the removed range.
   * Invalidates iterators referring to the removed elements.
   *
   * precondition: from and to must form a valid range for the list.
   * complexity: O(number of elements removed)
   */
  iterator erase(const_iterator from, const_iterator to) {
    return erase_and_dispose(from, to, NopDisposer());
  }

  reverse_iterator erase(const_reverse_iterator from, const_reverse_iterator to) {
    return erase_and_dispose(from, to, NopDisposer());
  }

  template<typename Disposer>
  iterator erase_and_dispose(const_iterator from, const_iterator to, Disposer disposer) {
    return erase_range_and_dispose<iterator>(from, to, disposer);
  }

  template<typename Disposer>
  reverse_iterator erase_and_dispose(const_reverse_iterator from,
                                     const_reverse_iterator to,
                                     Disposer disposer) {
    return erase_range_and_dispose<reverse_iterator>(from, to, disposer);
  }

private:

  template<typename Result, typename Iterator, typename Disposer>
  Result erase_range_and_dispose(Iterator from, Iterator to, Disposer disposer) {
    using IOps = Impl::IOps<Iterator>;
    assert_is_iterator(from);
    assert_is_iterator(to);
    if (from != to) {
      IOps::iter_attach(IOps::predecessor(*from), to);
      do {
        const_reference value = *from++;
        detach(value);
        disposer(disposer_arg(value));
      } while (from != to);
    }
    return make_iterator<Result>(to);
  }

public:

  /**
   * Conditionally removes elements from the list.  Successively calls the
   * predicate with a const_reference to each element of the list.  If the
   * result of such a call is true then that element is removed from the list.
   * Applies the disposer, if any, to each removed element.  The list may not
   * be in a consistent state when the disposer is called.  Returns the number
   * of removed elements.  Invalidates iterators referring to the removed
   * elements.
   *
   * complexity: O(length())
   */

  template<typename Predicate>
  size_type erase_if(Predicate predicate) {
    return erase_and_dispose_if(predicate, NopDisposer());
  }

  template<typename Predicate, typename Disposer>
  size_type erase_and_dispose_if(Predicate predicate, Disposer disposer) {
    const_iterator pos = cbegin();
    const_iterator end = cend();
    size_type removed = 0;
    while (pos != end) {
      const_reference v = *pos;
      if (predicate(v)) {
        pos = erase(pos);
        disposer(disposer_arg(v));
        ++removed;
      } else {
        ++pos;
      }
    }
    return removed;
  }

  /**
   * Removes all of the elements from the list.  Applies the disposer,
   * if any, to each element as it is removed.  The list may not be in
   * a consistent state when the disposer is called.  Invalidates all
   * non-end-of-list iterators for this list.
   *
   * postcondition: empty()
   * complexity: O(length())
   */
  void clear() {
    erase(begin(), end());
  }

  template<typename Disposer>
  void clear_and_dispose(Disposer disposer) {
    erase_and_dispose(begin(), end(), disposer);
  }

  /**
   * Inserts value into the list before pos.  Returns an iterator
   * referring to the newly inserted value.  Does not invalidate any
   * iterators.
   *
   * precondition: pos must be a valid iterator for the list.
   * precondition: value must not already be in a list using the same entry.
   * postcondition: ++result == pos
   * complexity: constant.
   */
  iterator insert(const_iterator pos, reference value) {
    return insert_impl<iterator>(pos, value);
  }

  reverse_iterator insert(const_reverse_iterator pos, reference value) {
    return insert_impl<reverse_iterator>(pos, value);
  }

private:

  template<typename Result, typename Iterator>
  Result insert_impl(Iterator pos, reference value) {
    assert(Impl::entry_list(get_entry(value)) == nullptr, "precondition");
    assert_is_iterator(pos);
    using IOps = Impl::IOps<Iterator>;
    IOps::attach(IOps::iter_predecessor(pos), value);
    IOps::attach(value, pos);
    DEBUG_ONLY(set_list(value, &_impl);)
    this->increase_size(1);
    return make_iterator_to<Result>(value);
  }

public:

  // The use of SFINAE with splice() and swap() is done for two reasons.
  //
  // It provides better compile-time error messages for certain kinds of usage
  // mistakes.  For example, if a splice from_list is not actually a list, or
  // a list with a different entry_key, we get some kind of "no
  // applicable function" failure at the call site, rather than some obscure
  // access failure deep inside the implementation of the operation.
  //
  // It ensures const-correctness at the API boundary, permitting the
  // implementation to be simpler by decaying to const iterators and
  // references in various places.

  /**
   * Transfers the elements of from_list in the range designated by
   * from and to to this list, inserted before pos.  Returns an
   * iterator referring to the head of the spliced in range.  Does
   * not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: from and to must form a valid range for from_list.
   * precondition: n is the distance between from and to.
   * precondition: pos is not in the range to transfer, i.e. either
   * - this != &from_list, or
   * - pos is reachable from to, or
   * - pos is not reachable from from.
   *
   * postcondition: iterators referring to elements in the transferred range
   * are valid iterators for this list rather than from_list.
   *
   * complexity: constant.
   */
  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  iterator splice(const_iterator pos,
                  FromList& from_list,
                  typename FromList::iterator from,
                  typename FromList::const_iterator to,
                  size_type n) {
    assert_is_iterator(pos);
    from_list.assert_is_iterator(from);
    from_list.assert_is_iterator(to);

    // Done if empty range.  This check simplifies remainder.
    if (from == to) {
      assert(n == 0, "incorrect range size: %zu, actual 0", n);
      return make_iterator<iterator>(pos);
    }

#ifdef ASSERT
    if (is_same_list(from_list)) {
      size_type count = check_self_splice_range(pos, from_list, from, to);
      assert(count == n, "incorrect range size: %zu, actual %zu", n, count);
    }
#endif // ASSERT

    // Done if already in desired position.  This check simplifies the normal case.
    if (is_same_list(from_list) && (pos == to)) {
      return make_iterator_to<iterator>(*from);
    }

    // Adjust sizes.  Could skip if same list, but not worth checking.
    from_list.decrease_size(n);
    this->increase_size(n);

    return splice_transfer(pos, from_list, from, to);
  }

  /**
   * Transfers the elements of from_list in the range designated by
   * from and to to this list, inserted before pos.  Returns an
   * iterator referring to the head of the spliced in range.  Does
   * not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: from and to must form a valid range for from_list.
   * precondition: pos is not in the range to transfer, i.e. either
   * - this != &from_list, or
   * - pos is reachable from to, or
   * - pos is not reachable from from.
   *
   * postcondition: iterators referring to elements in the transferred range
   * are valid iterators for this list rather than from_list.
   *
   * complexity: constant if either (a) this == &from_list, (b) neither
   * this nor from_list has a constant-time size() operation, or (c)
   * from_list has a constant-time size() operation and is being
   * transferred in its entirety; otherwise O(number of elements
   * transferred).
   */
  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  iterator splice(const_iterator pos,
                  FromList& from_list,
                  typename FromList::iterator from,
                  typename FromList::const_iterator to) {
    assert_is_iterator(pos);
    from_list.assert_is_iterator(from);
    from_list.assert_is_iterator(to);

    // Done if empty range.  This check simplifies remainder.
    if (from == to) {
      return make_iterator<iterator>(pos);
    }

#ifdef ASSERT
    if (is_same_list(from_list)) {
      check_self_splice_range(pos, from_list, from, to);
    }
#endif // ASSERT

    // Done if already in desired position.  This check simplifies the normal case.
    if (is_same_list(from_list) && (pos == to)) {
      return make_iterator_to<iterator>(*from);
    }

    // Adjust sizes if needed.
    if ((_has_size || from_list._has_size) && !is_same_list(from_list)) {
      splice_adjust_size(from_list, from, to);
    }

    return splice_transfer(pos, from_list, from, to);
  }

private:

#ifdef ASSERT
  template<typename FromList>
  size_type check_self_splice_range(const_iterator pos,
                                    FromList& from_list,
                                    const_iterator from,
                                    const_iterator to) {
    size_type count = 0;
    for (auto i = from; i != to; ++i) {
      assert(i != pos, "splice range includes destination");
      ++count;
    }
    return count;
  }
#endif // ASSERT

  // Select size-adjuster via SFINAE to only call size when available.
  // C++17 if-constexpr simplification candidate.

  template<typename FromList, ENABLE_IF(FromList::_has_size)>
  void splice_adjust_size(FromList& from_list,
                          typename FromList::const_iterator from,
                          typename FromList::const_iterator to) {
    size_type transferring;
    // If transferring entire list we can use constant-time size rather than
    // linear-time distance.
    if ((from == from_list.cbegin()) && (to == from_list.cend())) {
      transferring = from_list.size();
    } else {
      transferring = Impl::distance(from, to);
    }
    from_list.decrease_size(transferring);
    this->increase_size(transferring);
  }

  template<typename FromList, ENABLE_IF(!FromList::_has_size)>
  void splice_adjust_size(FromList& from_list,
                          typename FromList::const_iterator from,
                          typename FromList::const_iterator to) {
    size_type transferring = Impl::distance(from, to);
    from_list.decrease_size(transferring);
    this->increase_size(transferring);
  }

  template<typename FromList>
  iterator splice_transfer(const_iterator pos,
                           FromList& from_list,
                           typename FromList::iterator from,
                           typename FromList::const_iterator to) {
    assert(from != to, "precondition");
    using IOps = Impl::IOps<const_iterator>;
    // to is end of non-empty range, so has a dereferenceable predecessor.
    const_iterator to_pred = --const_iterator(to); // Fetch before clobbered
    // from is dereferenceable since it heads a non-empty range.
    const_reference from_value = *from;
#ifdef ASSERT
    if (!is_same_list(from_list)) {
      for (auto i = from; i != to; ++i) {
        set_list(*i, &_impl);
      }
    }
#endif // ASSERT
    IOps::iter_attach(IOps::predecessor(from_value), to);
    IOps::attach(IOps::iter_predecessor(pos), from_value);
    IOps::attach(*to_pred, pos);
    return make_iterator_to<iterator>(from_value);
   }

public:

  /**
   * Transfers all elements of from_list to this list, inserted before
   * pos.  Returns an iterator referring to the head of the spliced in
   * range.  Does not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: this != &from_list.
   *
   * postcondition: iterators referring to elements that were in
   * from_list are valid iterators for this list rather than
   * from_list.
   *
   * Complexity: constant if either (a) this does not have a
   * constant-time size() operation, or (b) from_list has a
   * constant-time size() operation; otherwise O(number of elements
   * transferred).
   */
  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  iterator splice(const_iterator pos, FromList& from_list) {
    assert(!is_same_list(from_list), "precondition");
    return splice_all(pos, from_list);
  }

private:

  // Select which splice with range overload to call, using SFINAE to only use
  // the one with a size argument when from_list has constant-time size.
  // C++17 if-constexpr simplification candidate.

  template<typename FromList, ENABLE_IF(FromList::_has_size)>
  iterator splice_all(const_iterator pos, FromList& from_list) {
    return splice(pos, from_list, from_list.begin(), from_list.end(), from_list.size());
  }

  template<typename FromList, ENABLE_IF(!FromList::_has_size)>
  iterator splice_all(const_iterator pos, FromList& from_list) {
     return splice(pos, from_list, from_list.begin(), from_list.end());
   }

public:

  /**
   * Transfers the element of from_list referred to by from to this
   * list, inserted before pos.  Returns an iterator referring to the
   * inserted element.  Does not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: from must be a dereferenceable iterator of from_list.
   * precondition: pos is not in the range to transfer, i.e. if
   * this == &from_list then pos != from.
   *
   * postcondition: iterators referring to the transferred element are
   * valid iterators for this list rather than from_list.
   *
   * complexity: constant.
   */
  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  iterator splice(const_iterator pos,
                  FromList& from_list,
                  typename FromList::iterator from) {
    using IOps = Impl::IOps<const_iterator>;

    assert_is_iterator(pos);
    from_list.assert_is_iterator(from);

#ifdef ASSERT
    // Transfer element to this list, or verify pos not in [from, to).
    if (is_same_list(from_list)) {
      assert(from != pos, "Splice range includes destination");
    } else {
      set_list(*from, &_impl);
    }
#endif // ASSERT

    const_reference from_value = *from;

    // Remove from_value from from_list.
    IOps::iter_attach(IOps::predecessor(from_value), IOps::successor(from_value));
    from_list.decrease_size(1);

    // Add from_value to this list before pos.
    IOps::attach(IOps::iter_predecessor(pos), from_value);
    IOps::attach(from_value, pos);
    this->increase_size(1);

    return make_iterator_to<iterator>(from_value);
  }

  /**
   * Exchange the elements of this list and other, maintaining the order of
   * the elements.  Does not invalidate any iterators.
   *
   * precondition: this and other are different lists.
   *
   * postcondition: iterators referring to elements in this list become valid
   * iterators for other, and vice versa.
   *
   * complexity: if one of the lists has constant-time size and the other does
   * not, then O(number of elements in the list without constant-time size);
   * otherwise constant (when neither or both lists have constant-time size).
   */
  template<typename OtherList, ENABLE_IF(can_swap<OtherList>())>
  void swap(OtherList& other) {
    assert(!is_same_list(other), "self-swap");
    swap_impl(*this, other);
  }

private:

  // Select swap implementation using SFINAE, based on whether the lists are
  // size-tracking.
  // C++17 if-constexpr simplification candidate.

  // If list2 has constant-time size, we can use that when transferring its
  // contents to list1.  First transfer all of list1 to the front of list2,
  // which is constant-time if list1 has size, else linear.  Then transfer the
  // original contents of list2 to list1 using the original size, which is
  // constant-time.
  template<typename List1, typename List2, ENABLE_IF(List2::_has_size)>
  static void swap_impl(List1& list1, List2& list2) {
    size_type old_size = list2.size();
    auto old_start = list2.begin();
    list2.splice(old_start, list1);
    list1.splice(list1.end(), list2, old_start, list2.end(), old_size);
  }

  // If neither list has constant-time size, then transfer all of list1 to the
  // front of list2, then transfer the original contents of list2 to list1.
  // (Or vice versa.)  Both are constant-time operations.
  template<typename List1, typename List2,
           ENABLE_IF(!List1::_has_size), ENABLE_IF(!List2::_has_size)>
  static void swap_impl(List1& list1, List2& list2) {
    auto old_start = list2.begin();
    list2.splice(old_start, list1);
    list1.splice(list1.end(), list2, old_start, list2.end());
  }

  // If list1 has constant-time size and list2 doesn't, then reverse the order
  // of the arguments to call the overload where list2 does, getting a linear
  // time operation.
  template<typename List1, typename List2,
           ENABLE_IF(List1::_has_size), ENABLE_IF(!List2::_has_size)>
  static void swap_impl(List1& list1, List2& list2) {
    swap_impl(list2, list1);
  }

public:

  /**
   * Returns a [const_][reverse_]iterator referring to value.
   *
   * precondition: value must be an element of the list.
   * complexity: constant.
   */
  iterator iterator_to(reference value) {
    return make_iterator_to<iterator>(value);
  }

  const_iterator iterator_to(const_reference value) const {
    return const_iterator_to(value);
  }

  const_iterator const_iterator_to(const_reference value) const {
    return make_iterator_to<const_iterator>(value);
  }

  reverse_iterator reverse_iterator_to(reference value) {
    return make_iterator_to<reverse_iterator>(value);
  }

  const_reverse_iterator reverse_iterator_to(const_reference value) const {
    return const_reverse_iterator_to(value);
  }

  const_reverse_iterator const_reverse_iterator_to(const_reference value) const {
    return make_iterator_to<const_reverse_iterator>(value);
  }

private:
  Impl _impl;

  template<typename OtherList>
  bool is_same_list(const OtherList& other) const {
    return &_impl == &other._impl;
  }

  template<typename Iterator>
  void assert_is_iterator(const Iterator& i) const {
    using IOps = Impl::IOps<Iterator>;
    assert(IOps::list_ptr(i) == &_impl,
           "Iterator " PTR_FORMAT " not for this list " PTR_FORMAT,
           p2i(IOps::encoded_value(i)), p2i(this));
  }

  void assert_is_element(const_reference value) const {
    assert(Impl::entry_list(get_entry(value)) == &_impl,
           "Value " PTR_FORMAT " not in this list " PTR_FORMAT,
           p2i(&value), p2i(this));
  }

#ifdef ASSERT
  void set_list(const_reference value, Impl* list) {
    Impl::set_entry_list(get_entry(value), list);
  }
#endif

  template<typename Result, typename From>
  Result make_iterator(From i) const {
    assert_is_iterator(i);
    return Impl::IOps<Result>::make_iterator(i);
  }

  // This can break the rules about putting const elements in non-const
  // iterators or lists.  It is up to callers to ensure that doesn't happen
  // and result in implicitly casting away const of the passed argument.
  template<typename Iterator>
  Iterator make_iterator_to(const_reference value) const {
    assert_is_element(value);
    return Impl::IOps<Iterator>::make_iterator_to(value);
  }

  struct NopDisposer {
    void operator()(pointer) const {}
  };

  // The pointer type is const-qualified per to the elements of the list, so
  // it's okay to possibly cast away const when disposing.
  static pointer disposer_arg(const_reference value) {
    return const_cast<pointer>(&value);
  }

  void detach(const_reference value) {
    assert_is_element(value);
    Impl::detach(get_entry(value));
    this->decrease_size(1);
  }

  static const Entry& get_entry(const_reference v) {
    return Impl::get_entry<entry_key>(v);
  }
};

#endif // SHARE_UTILITIES_INTRUSIVELIST_HPP
