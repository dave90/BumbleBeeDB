//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_internal_page.h
//
// Identification: src/include/storage/page/b_plus_tree_internal_page.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>

#include "common/config.h"
#include "storage/page/b_plus_tree_page.h"

namespace bumblebee {

#define B_PLUS_TREE_INTERNAL_PAGE_TYPE BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>
#define INTERNAL_PAGE_HEADER_SIZE 12
#define INTERNAL_PAGE_SLOT_CNT \
  ((PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (static_cast<int>(sizeof(KeyType) + sizeof(ValueType))))

/**
 * @brief An internal B+ tree page: `n` keys and `n + 1` child page ids.
 *
 * The child pointer `PAGE_ID(i)` covers the subtree whose keys `K` satisfy `K(i) <= K < K(i+1)`.
 * Since there is one more child than key, `key_array_[0]` is always invalid and every search skips it.
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTreeInternalPage : public BPlusTreePage {
 public:
  BPlusTreeInternalPage() = delete;
  BPlusTreeInternalPage(const BPlusTreeInternalPage &other) = delete;

  void Init(int max_size = INTERNAL_PAGE_SLOT_CNT);

  auto KeyAt(int index) const -> KeyType;
  void SetKeyAt(int index, const KeyType &key);

  auto ValueIndex(const ValueType &value) const -> int;
  auto ValueAt(int index) const -> ValueType;
  void SetValueAt(int index, const ValueType &value);

  void Insert(size_t idx, const KeyType &key, const ValueType &value);
  void Insert(KeyComparator comparator, const KeyType &key, const ValueType &value);
  void Split(BPlusTreeInternalPage &node, page_id_t page_id);
  void Remove(KeyComparator comparator, const KeyType &key, int key_idx = -1);
  void Remove(int key_idx);
  auto StealValues(KeyComparator comparator, BPlusTreeInternalPage &node, bool sib_left, const KeyType &parent_sep,
                   KeyType &new_sep) -> bool;
  void InsertAllValues(KeyComparator comparator, BPlusTreeInternalPage &node, const KeyType &parent_sep);

 private:
  KeyType key_array_[INTERNAL_PAGE_SLOT_CNT];
  ValueType page_id_array_[INTERNAL_PAGE_SLOT_CNT];
};

}  // namespace bumblebee
