//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_leaf_page.cpp
//
// Identification: src/storage/page/b_plus_tree_leaf_page.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/page/b_plus_tree_leaf_page.h"

#include "common/macros.h"
#include "storage/table/rid.h"

namespace bumblebee {

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Init(int max_size) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetMaxSize(max_size);
  SetSize(0);
  SetNextPageId(INVALID_PAGE_ID);
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::GetNextPageId() const -> page_id_t { return next_page_id_; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::KeyAt(int index) const -> KeyType { return key_array_[index]; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::SetKeyAt(int index, const KeyType &key) { key_array_[index] = key; }

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::ValueAt(int index) const -> ValueType { return rid_array_[index]; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::SetValueAt(int index, const ValueType &value) { rid_array_[index] = value; }

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::ContainsKey(KeyComparator comparator, const KeyType &key, int key_idx) const
    -> KeyStatus {
  int idx = (key_idx == -1) ? BisectRight(*this, key, comparator) - 1 : key_idx;
  if (idx >= 0 && comparator(KeyAt(idx), key) == 0) {
    return KeyStatus::LIVE;
  }
  return KeyStatus::ABSENT;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Insert(size_t idx, const KeyType &key, const ValueType &value) {
  size_t size = GetSize();
  BUMBLEBEE_ASSERT(static_cast<int>(size) < GetMaxSize(), "leaf is full");
  for (size_t i = size; i > idx; --i) {
    key_array_[i] = key_array_[i - 1];
    rid_array_[i] = rid_array_[i - 1];
  }
  key_array_[idx] = key;
  rid_array_[idx] = value;
  ChangeSizeBy(1);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Insert(KeyComparator comparator, const KeyType &key, const ValueType &value) {
  auto idx = BisectRight(*this, key, comparator);
  Insert(idx, key, value);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Split(BPlusTreeLeafPage &node, page_id_t page_id) {
  size_t size = GetSize();
  BUMBLEBEE_ASSERT(size == static_cast<size_t>(GetMaxSize()), "Split called on a non-full node");
  BUMBLEBEE_ASSERT(node.GetSize() == 0, "Split target must be empty");
  size_t mid = size / 2;
  size_t idx = 0;
  for (auto i = mid; i < size; ++i) {
    node.key_array_[idx] = key_array_[i];
    node.rid_array_[idx] = rid_array_[i];
    ++idx;
  }
  node.next_page_id_ = next_page_id_;
  next_page_id_ = page_id;
  node.ChangeSizeBy(static_cast<int>(idx));
  ChangeSizeBy(-static_cast<int>(idx));
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::Remove(KeyComparator comparator, const KeyType &key, int key_idx) -> bool {
  auto idx = (key_idx < 0) ? BisectRight(*this, key, comparator) - 1 : key_idx;
  if (idx < 0 || comparator(KeyAt(idx), key) != 0) {
    return false;
  }
  RemoveAt(idx);
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::RemoveAt(int idx) {
  for (int i = idx + 1; i < GetSize(); ++i) {
    key_array_[i - 1] = key_array_[i];
    rid_array_[i - 1] = rid_array_[i];
  }
  ChangeSizeBy(-1);
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::StealValues(KeyComparator comparator, BPlusTreeLeafPage &sib_node, bool sib_left,
                                             const KeyType &parent_sep, KeyType &new_sep) -> bool {
  if (GetSize() >= GetMinSize()) {
    return false;
  }
  if (sib_node.GetSize() - 1 < sib_node.GetMinSize()) {
    return false;  // sibling cannot spare an entry
  }
  if (sib_left) {
    int last = sib_node.GetSize() - 1;
    Insert(0, sib_node.KeyAt(last), sib_node.ValueAt(last));
    sib_node.RemoveAt(last);
    new_sep = KeyAt(0);
  } else {
    Insert(static_cast<size_t>(GetSize()), sib_node.KeyAt(0), sib_node.ValueAt(0));
    sib_node.RemoveAt(0);
    new_sep = sib_node.KeyAt(0);
  }
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::InsertAllValues(KeyComparator comparator, BPlusTreeLeafPage &sib_node,
                                                 const KeyType &parent_sep) {
  BUMBLEBEE_ASSERT(GetSize() + sib_node.GetSize() <= GetMaxSize(), "InsertAllValues: merged size exceeds max");
  size_t at = GetSize();
  for (int j = 0; j < sib_node.GetSize(); j++) {
    key_array_[at] = sib_node.KeyAt(j);
    rid_array_[at] = sib_node.ValueAt(j);
    ++at;
  }
  SetSize(static_cast<int>(at));
  SetNextPageId(sib_node.GetNextPageId());
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::GetNextIdx(int idx) const -> int {
  return (idx + 1 < GetSize()) ? idx + 1 : -1;
}

template class BPlusTreeLeafPage<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTreeLeafPage<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTreeLeafPage<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTreeLeafPage<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bumblebee
