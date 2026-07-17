//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_internal_page.cpp
//
// Identification: src/storage/page/b_plus_tree_internal_page.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/page/b_plus_tree_internal_page.h"

#include "common/macros.h"
#include "storage/table/rid.h"

namespace bumblebee {

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Init(int max_size) {
  SetMaxSize(max_size);
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::KeyAt(int index) const -> KeyType { return key_array_[index]; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetKeyAt(int index, const KeyType &key) { key_array_[index] = key; }

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueIndex(const ValueType &value) const -> int {
  for (int i = 0; i < GetSize(); i++) {
    if (page_id_array_[i] == value) {
      return i;
    }
  }
  return -1;
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueAt(int index) const -> ValueType { return page_id_array_[index]; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetValueAt(int index, const ValueType &value) { page_id_array_[index] = value; }

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Insert(size_t idx, const KeyType &key, const ValueType &value) {
  size_t size = GetSize();
  BUMBLEBEE_ASSERT(static_cast<int>(size) < GetMaxSize(), "internal page is full");
  for (size_t i = size; i > idx; --i) {
    key_array_[i] = key_array_[i - 1];
    page_id_array_[i] = page_id_array_[i - 1];
  }
  key_array_[idx] = key;
  page_id_array_[idx] = value;
  ChangeSizeBy(1);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Insert(KeyComparator comparator, const KeyType &key, const ValueType &value) {
  auto idx = BisectRight(*this, key, comparator, 1);
  Insert(idx, key, value);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Remove(KeyComparator comparator, const KeyType &key, int key_idx) {
  auto idx = (key_idx < 0) ? BisectRight(*this, key, comparator, 1) - 1 : key_idx;
  if (idx < 0 || comparator(KeyAt(idx), key) != 0) {
    return;
  }
  Remove(idx);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Remove(int key_idx) {
  for (int i = key_idx + 1; i < GetSize(); ++i) {
    key_array_[i - 1] = key_array_[i];
    page_id_array_[i - 1] = page_id_array_[i];
  }
  ChangeSizeBy(-1);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Split(BPlusTreeInternalPage &node, page_id_t page_id) {
  size_t size = GetSize();
  BUMBLEBEE_ASSERT(size == static_cast<size_t>(GetMaxSize()), "Split called on a non-full node");
  BUMBLEBEE_ASSERT(node.GetSize() == 0, "Split target must be empty");
  size_t mid = size / 2;
  size_t idx = 0;
  for (auto i = mid; i < size; ++i) {
    node.key_array_[idx] = key_array_[i];
    node.page_id_array_[idx] = page_id_array_[i];
    ++idx;
  }
  node.ChangeSizeBy(static_cast<int>(idx));
  ChangeSizeBy(-static_cast<int>(idx));
}

INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::StealValues(KeyComparator comparator, BPlusTreeInternalPage &sib_node,
                                                 bool sib_left, const KeyType &parent_sep, KeyType &new_sep) -> bool {
  if (sib_node.GetSize() - 1 < sib_node.GetMinSize()) {
    return false;  // sibling cannot spare a child
  }
  if (sib_left) {
    // Take the sibling's last child; pull `parent_sep` down as the separator of the moved pointer.
    int last = sib_node.GetSize() - 1;
    for (int i = GetSize(); i > 0; --i) {
      key_array_[i] = key_array_[i - 1];
      page_id_array_[i] = page_id_array_[i - 1];
    }
    page_id_array_[0] = sib_node.ValueAt(last);
    key_array_[1] = parent_sep;
    ChangeSizeBy(1);
    new_sep = sib_node.KeyAt(last);
    sib_node.Remove(last);
  } else {
    // Take the sibling's first child; pull `parent_sep` down as its separator.
    Insert(static_cast<size_t>(GetSize()), parent_sep, sib_node.ValueAt(0));
    new_sep = sib_node.KeyAt(1);
    sib_node.Remove(0);
  }
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::InsertAllValues(KeyComparator comparator, BPlusTreeInternalPage &sib_node,
                                                     const KeyType &parent_sep) {
  BUMBLEBEE_ASSERT(GetSize() + sib_node.GetSize() <= GetMaxSize(), "InsertAllValues: merged size exceeds max");
  size_t at = GetSize();
  key_array_[at] = parent_sep;
  page_id_array_[at] = sib_node.ValueAt(0);
  ++at;
  for (int j = 1; j < sib_node.GetSize(); j++) {
    key_array_[at] = sib_node.KeyAt(j);
    page_id_array_[at] = sib_node.ValueAt(j);
    ++at;
  }
  SetSize(static_cast<int>(at));
}

template class BPlusTreeInternalPage<GenericKey<4>, page_id_t, GenericComparator<4>>;
template class BPlusTreeInternalPage<GenericKey<8>, page_id_t, GenericComparator<8>>;
template class BPlusTreeInternalPage<GenericKey<16>, page_id_t, GenericComparator<16>>;
template class BPlusTreeInternalPage<GenericKey<32>, page_id_t, GenericComparator<32>>;
template class BPlusTreeInternalPage<GenericKey<64>, page_id_t, GenericComparator<64>>;

}  // namespace bumblebee
