//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_leaf_page.h
//
// Identification: src/include/storage/page/b_plus_tree_leaf_page.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>

#include "common/config.h"
#include "storage/page/b_plus_tree_page.h"

namespace bumblebee {

#define B_PLUS_TREE_LEAF_PAGE_TYPE BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>
#define LEAF_PAGE_HEADER_SIZE 16
#define LEAF_PAGE_SLOT_CNT \
  ((PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (static_cast<int>(sizeof(KeyType) + sizeof(ValueType))))

/** Whether a key is present and live, or absent. */
enum class KeyStatus { LIVE, ABSENT };

/**
 * @brief A leaf B+ tree page: sorted keys with their record ids, plus a link to the next leaf.
 *
 * This is the canonical leaf — deletions remove the entry immediately (no tombstone buffer). Only
 * unique keys are supported.
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTreeLeafPage : public BPlusTreePage {
 public:
  BPlusTreeLeafPage() = delete;
  BPlusTreeLeafPage(const BPlusTreeLeafPage &other) = delete;

  void Init(int max_size = LEAF_PAGE_SLOT_CNT);

  auto GetNextPageId() const -> page_id_t;
  void SetNextPageId(page_id_t next_page_id);

  auto KeyAt(int index) const -> KeyType;
  void SetKeyAt(int index, const KeyType &key);
  auto ValueAt(int index) const -> ValueType;
  void SetValueAt(int index, const ValueType &value);

  auto ContainsKey(KeyComparator comparator, const KeyType &key, int key_idx = -1) const -> KeyStatus;

  void Insert(size_t idx, const KeyType &key, const ValueType &value);
  void Insert(KeyComparator comparator, const KeyType &key, const ValueType &value);
  void Split(BPlusTreeLeafPage &node, page_id_t page_id);
  auto Remove(KeyComparator comparator, const KeyType &key, int key_idx = -1) -> bool;

  auto StealValues(KeyComparator comparator, BPlusTreeLeafPage &node, bool sib_left, const KeyType &parent_sep,
                   KeyType &new_sep) -> bool;
  void InsertAllValues(KeyComparator comparator, BPlusTreeLeafPage &node, const KeyType &parent_sep);

  /** @return The next live index after `idx`, or -1 if none (canonical: every entry is live). */
  auto GetNextIdx(int idx) const -> int;

 private:
  void RemoveAt(int idx);

  page_id_t next_page_id_;
  KeyType key_array_[LEAF_PAGE_SLOT_CNT];
  ValueType rid_array_[LEAF_PAGE_SLOT_CNT];
};

}  // namespace bumblebee
