//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/bits.h"
#include "td/utils/common.h"
#include "td/utils/Random.h"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <new>
#include <utility>

namespace td {

template <class KeyT>
bool is_key_empty(const KeyT &key) {
  return key == KeyT();
}

inline uint32 randomize_hash(size_t h) {
  auto result = static_cast<uint32>(h & 0xFFFFFFFF);
  result ^= result >> 16;
  result *= 0x85ebca6b;
  result ^= result >> 13;
  result *= 0xc2b2ae35;
  result ^= result >> 16;
  return result;
}

template <class KeyT, class ValueT>
struct MapNode {
  using first_type = KeyT;
  using second_type = ValueT;
  using public_key_type = KeyT;
  using public_type = MapNode;

  KeyT first{};
  union {
    ValueT second;
  };

  const KeyT &key() const {
    return first;
  }

  MapNode &get_public() {
    return *this;
  }

  MapNode() {
  }
  MapNode(KeyT key, ValueT value) : first(std::move(key)) {
    new (&second) ValueT(std::move(value));
    DCHECK(!empty());
  }
  MapNode(const MapNode &other) = delete;
  MapNode &operator=(const MapNode &other) = delete;
  MapNode(MapNode &&other) noexcept {
    *this = std::move(other);
  }
  void operator=(MapNode &&other) noexcept {
    DCHECK(empty());
    DCHECK(!other.empty());
    first = std::move(other.first);
    other.first = KeyT{};
    new (&second) ValueT(std::move(other.second));
    other.second.~ValueT();
  }
  ~MapNode() {
    if (!empty()) {
      second.~ValueT();
    }
  }

  void copy_from(const MapNode &other) {
    DCHECK(empty());
    DCHECK(!other.empty());
    first = other.first;
    new (&second) ValueT(other.second);
  }

  bool empty() const {
    return is_key_empty(key());
  }

  void clear() {
    DCHECK(!empty());
    first = KeyT();
    second.~ValueT();
    DCHECK(empty());
  }

  template <class... ArgsT>
  void emplace(KeyT key, ArgsT &&...args) {
    DCHECK(empty());
    first = std::move(key);
    new (&second) ValueT(std::forward<ArgsT>(args)...);
    DCHECK(!empty());
  }
};

template <class KeyT>
struct SetNode {
  using public_key_type = KeyT;
  using public_type = KeyT;
  using second_type = KeyT;  // TODO: remove second_type?

  KeyT first{};

  const KeyT &key() const {
    return first;
  }

  KeyT &get_public() {
    return first;
  }

  SetNode() = default;
  explicit SetNode(KeyT key) : first(std::move(key)) {
  }
  SetNode(const SetNode &other) = delete;
  SetNode &operator=(const SetNode &other) = delete;
  SetNode(SetNode &&other) noexcept {
    *this = std::move(other);
  }
  void operator=(SetNode &&other) noexcept {
    DCHECK(empty());
    DCHECK(!other.empty());
    first = std::move(other.first);
    other.first = KeyT{};
  }
  ~SetNode() = default;

  void copy_from(const SetNode &other) {
    DCHECK(empty());
    DCHECK(!other.empty());
    first = other.first;
  }

  bool empty() const {
    return is_key_empty(key());
  }

  void clear() {
    first = KeyT();
    DCHECK(empty());
  }

  void emplace(KeyT key) {
    first = std::move(key);
  }
};

template <class NodeT, class HashT, class EqT>
class FlatHashTable {
  struct FlatHashTableInner {
    uint32 used_node_count_;
    uint32 bucket_count_mask_;
    NodeT nodes_[1];
  };

  static constexpr size_t OFFSET = 2 * sizeof(uint32);

  static NodeT *allocate_nodes(uint32 size) {
    DCHECK(size >= 8);
    DCHECK((size & (size - 1)) == 0);
    CHECK(size <= min(static_cast<uint32>(1) << 29, static_cast<uint32>((0x7FFFFFFF - OFFSET) / sizeof(NodeT))));
    auto inner = static_cast<FlatHashTableInner *>(std::malloc(OFFSET + sizeof(NodeT) * size));
    NodeT *nodes = &inner->nodes_[0];
    for (uint32 i = 0; i < size; i++) {
      new (nodes + i) NodeT();
    }
    // inner->used_node_count_ = 0;
    inner->bucket_count_mask_ = size - 1;
    return nodes;
  }

  static void clear_inner(FlatHashTableInner *inner) {
    auto size = inner->bucket_count_mask_ + 1;
    NodeT *nodes = &inner->nodes_[0];
    for (uint32 i = 0; i < size; i++) {
      nodes[i].~NodeT();
    }
    std::free(inner);
  }

  inline FlatHashTableInner *get_inner() {
    DCHECK(nodes_ != nullptr);
    return reinterpret_cast<FlatHashTableInner *>(reinterpret_cast<char *>(nodes_) - OFFSET);
  }

  inline const FlatHashTableInner *get_inner() const {
    DCHECK(nodes_ != nullptr);
    return reinterpret_cast<const FlatHashTableInner *>(reinterpret_cast<const char *>(nodes_) - OFFSET);
  }

  inline uint32 &used_node_count() {
    return get_inner()->used_node_count_;
  }

  inline uint32 get_used_node_count() const {
    return get_inner()->used_node_count_;
  }

  inline uint32 get_bucket_count_mask() const {
    return get_inner()->bucket_count_mask_;
  }

 public:
  using KeyT = typename NodeT::public_key_type;
  using key_type = typename NodeT::public_key_type;
  using value_type = typename NodeT::public_type;

  struct Iterator {
    using iterator_category = std::bidirectional_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = FlatHashTable::value_type;
    using pointer = value_type *;
    using reference = value_type &;

    friend class FlatHashTable;
    Iterator &operator++() {
      DCHECK(it_ != nullptr);
      do {
        if (unlikely(++it_ == end_)) {
          it_ = begin_;
        }
        if (unlikely(it_ == start_)) {
          it_ = nullptr;
          break;
        }
      } while (it_->empty());
      return *this;
    }
    reference operator*() {
      return it_->get_public();
    }
    pointer operator->() {
      return &*it_;
    }
    bool operator==(const Iterator &other) const {
      DCHECK(begin_ == other.begin_);
      DCHECK(end_ == other.end_);
      return it_ == other.it_;
    }
    bool operator!=(const Iterator &other) const {
      DCHECK(begin_ == other.begin_);
      DCHECK(end_ == other.end_);
      return it_ != other.it_;
    }

    Iterator() = default;
    Iterator(NodeT *it, FlatHashTable *map)
        : it_(it), begin_(map->nodes_), start_(it_), end_(map->nodes_ + map->bucket_count()) {
    }

   private:
    NodeT *it_ = nullptr;
    NodeT *begin_ = nullptr;
    NodeT *start_ = nullptr;
    NodeT *end_ = nullptr;
  };

  struct ConstIterator {
    using iterator_category = std::bidirectional_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = FlatHashTable::value_type;
    using pointer = const value_type *;
    using reference = const value_type &;

    friend class FlatHashTable;
    ConstIterator &operator++() {
      ++it_;
      return *this;
    }
    reference operator*() {
      return *it_;
    }
    pointer operator->() {
      return &*it_;
    }
    bool operator==(const ConstIterator &other) const {
      return it_ == other.it_;
    }
    bool operator!=(const ConstIterator &other) const {
      return it_ != other.it_;
    }

    ConstIterator() = default;
    ConstIterator(Iterator it) : it_(std::move(it)) {
    }

   private:
    Iterator it_;
  };
  using iterator = Iterator;
  using const_iterator = ConstIterator;

  FlatHashTable() = default;
  FlatHashTable(const FlatHashTable &other) {
    assign(other);
  }
  void operator=(const FlatHashTable &other) {
    clear();
    assign(other);
  }

  FlatHashTable(std::initializer_list<NodeT> nodes) {
    if (nodes.size() == 0) {
      return;
    }
    reserve(nodes.size());
    uint32 used_nodes = 0;
    for (auto &new_node : nodes) {
      CHECK(!new_node.empty());
      auto bucket = calc_bucket(new_node.key());
      while (true) {
        auto &node = nodes_[bucket];
        if (node.empty()) {
          node.copy_from(new_node);
          used_nodes++;
          break;
        }
        if (EqT()(node.key(), new_node.key())) {
          break;
        }
        next_bucket(bucket);
      }
    }
    used_node_count() = used_nodes;
  }

  FlatHashTable(FlatHashTable &&other) noexcept : nodes_(other.nodes_) {
    other.nodes_ = nullptr;
  }
  void operator=(FlatHashTable &&other) noexcept {
    clear();
    nodes_ = other.nodes_;
    other.nodes_ = nullptr;
  }
  void swap(FlatHashTable &other) noexcept {
    std::swap(nodes_, other.nodes_);
  }
  ~FlatHashTable() = default;

  uint32 bucket_count() const {
    return unlikely(nodes_ == nullptr) ? 0 : get_bucket_count_mask() + 1;
  }

  Iterator find(const KeyT &key) {
    if (unlikely(nodes_ == nullptr) || is_key_empty(key)) {
      return end();
    }
    auto bucket = calc_bucket(key);
    while (true) {
      auto &node = nodes_[bucket];
      if (EqT()(node.key(), key)) {
        return Iterator{&node, this};
      }
      if (node.empty()) {
        return end();
      }
      next_bucket(bucket);
    }
  }

  ConstIterator find(const KeyT &key) const {
    return ConstIterator(const_cast<FlatHashTable *>(this)->find(key));
  }

  size_t size() const {
    return unlikely(nodes_ == nullptr) ? 0 : get_used_node_count();
  }

  bool empty() const {
    return unlikely(nodes_ == nullptr) || get_used_node_count() == 0;
  }

  Iterator begin() {
    if (empty()) {
      return end();
    }
    auto bucket = Random::fast_uint32() & get_bucket_count_mask();
    while (nodes_[bucket].empty()) {
      next_bucket(bucket);
    }
    return Iterator(nodes_ + bucket, this);
  }
  Iterator end() {
    return Iterator(nullptr, this);
  }

  ConstIterator begin() const {
    return ConstIterator(const_cast<FlatHashTable *>(this)->begin());
  }
  ConstIterator end() const {
    return ConstIterator(const_cast<FlatHashTable *>(this)->end());
  }

  void reserve(size_t size) {
    if (size == 0) {
      return;
    }
    CHECK(size <= (1u << 29));
    uint32 want_size = normalize(static_cast<uint32>(size) * 5 / 3 + 1);
    if (want_size > bucket_count()) {
      resize(want_size);
    }
  }

  template <class... ArgsT>
  std::pair<Iterator, bool> emplace(KeyT key, ArgsT &&...args) {
    try_grow();
    CHECK(!is_key_empty(key));
    auto bucket = calc_bucket(key);
    while (true) {
      auto &node = nodes_[bucket];
      if (EqT()(node.key(), key)) {
        return {Iterator(&node, this), false};
      }
      if (node.empty()) {
        node.emplace(std::move(key), std::forward<ArgsT>(args)...);
        used_node_count()++;
        return {Iterator(&node, this), true};
      }
      next_bucket(bucket);
    }
  }

  std::pair<Iterator, bool> insert(KeyT key) {
    return emplace(std::move(key));
  }

  template <class ItT>
  void insert(ItT begin, ItT end) {
    for (; begin != end; ++begin) {
      emplace(*begin);
    }
  }

  template <class T = typename NodeT::second_type>
  T &operator[](const KeyT &key) {
    return emplace(key).first->second;
  }

  size_t erase(const KeyT &key) {
    auto it = find(key);
    if (it == end()) {
      return 0;
    }
    erase(it);
    return 1;
  }

  size_t count(const KeyT &key) const {
    return find(key) != end();
  }

  void clear() {
    if (nodes_ != nullptr) {
      clear_inner(get_inner());
      nodes_ = nullptr;
    }
  }

  void erase(Iterator it) {
    DCHECK(it != end());
    DCHECK(!it.it_->empty());
    erase_node(it.it_);
    try_shrink();
  }

  template <class F>
  void remove_if(F &&f) {
    if (empty()) {
      return;
    }

    auto it = begin().it_;
    auto end = nodes_ + bucket_count();
    while (it != end && !it->empty()) {
      ++it;
    }
    if (it == end) {
      do {
        --it;
      } while (!it->empty());
    }
    auto first_empty = it;
    while (it != end) {
      if (!it->empty() && f(it->get_public())) {
        erase_node(it);
      } else {
        ++it;
      }
    }
    for (it = nodes_; it != first_empty;) {
      if (!it->empty() && f(it->get_public())) {
        erase_node(it);
      } else {
        ++it;
      }
    }
    try_shrink();
  }

 private:
  NodeT *nodes_ = nullptr;

  void assign(const FlatHashTable &other) {
    if (other.size() == 0) {
      return;
    }
    resize(other.bucket_count());
    for (const auto &new_node : other) {
      auto bucket = calc_bucket(new_node.key());
      while (true) {
        auto &node = nodes_[bucket];
        if (node.empty()) {
          node.copy_from(new_node);
          break;
        }
        next_bucket(bucket);
      }
    }
    used_node_count() = other.get_used_node_count();
  }

  void try_grow() {
    if (unlikely(nodes_ == nullptr)) {
      resize(8);
    } else if (unlikely(get_used_node_count() * 5 > get_bucket_count_mask() * 3)) {
      resize(2 * get_bucket_count_mask() + 2);
    }
  }

  void try_shrink() {
    DCHECK(nodes_ != nullptr);
    if (unlikely(get_used_node_count() * 10 < get_bucket_count_mask() && get_bucket_count_mask() > 7)) {
      resize(normalize((get_used_node_count() + 1) * 5 / 3 + 1));
    }
  }

  static uint32 normalize(uint32 size) {
    return td::max(static_cast<uint32>(1) << (32 - count_leading_zeroes32(size)), static_cast<uint32>(8));
  }

  uint32 calc_bucket(const KeyT &key) const {
    return randomize_hash(HashT()(key)) & get_bucket_count_mask();
  }

  inline void next_bucket(uint32 &bucket) const {
    bucket = (bucket + 1) & get_bucket_count_mask();
  }

  void resize(uint32 new_size) {
    if (unlikely(nodes_ == nullptr)) {
      nodes_ = allocate_nodes(new_size);
      used_node_count() = 0;
      return;
    }

    auto old_nodes = nodes_;
    uint32 old_size = get_used_node_count();
    uint32 old_bucket_count = get_bucket_count_mask() + 1;
    nodes_ = allocate_nodes(new_size);
    used_node_count() = old_size;

    for (uint32 i = 0; i < old_bucket_count; i++) {
      auto &old_node = old_nodes[i];
      if (old_node.empty()) {
        continue;
      }
      auto bucket = calc_bucket(old_node.key());
      while (!nodes_[bucket].empty()) {
        next_bucket(bucket);
      }
      nodes_[bucket] = std::move(old_node);
    }
  }

  void erase_node(NodeT *it) {
    DCHECK(nodes_ <= it && static_cast<size_t>(it - nodes_) < bucket_count());
    uint32 empty_i = static_cast<uint32>(it - nodes_);
    auto empty_bucket = empty_i;
    nodes_[empty_bucket].clear();
    used_node_count()--;

    for (uint32 test_i = empty_i + 1;; test_i++) {
      auto test_bucket = test_i;
      if (test_bucket >= bucket_count()) {
        test_bucket -= bucket_count();
      }

      if (nodes_[test_bucket].empty()) {
        break;
      }

      auto want_i = calc_bucket(nodes_[test_bucket].key());
      if (want_i < empty_i) {
        want_i += static_cast<uint32>(bucket_count());
      }

      if (want_i <= empty_i || want_i > test_i) {
        nodes_[empty_bucket] = std::move(nodes_[test_bucket]);
        empty_i = test_i;
        empty_bucket = test_bucket;
      }
    }
  }
};

template <class KeyT, class ValueT, class HashT = std::hash<KeyT>, class EqT = std::equal_to<KeyT>>
using FlatHashMapImpl = FlatHashTable<MapNode<KeyT, ValueT>, HashT, EqT>;
template <class KeyT, class HashT = std::hash<KeyT>, class EqT = std::equal_to<KeyT>>
using FlatHashSetImpl = FlatHashTable<SetNode<KeyT>, HashT, EqT>;

}  // namespace td
