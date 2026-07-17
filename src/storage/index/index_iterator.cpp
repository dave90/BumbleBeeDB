//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// index_iterator.cpp
//
// Identification: src/storage/index/index_iterator.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/index/index_iterator.h"

#include <utility>

#include "storage/table/rid.h"

namespace bumblebee {

INDEX_TEMPLATE_ARGUMENTS
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator() = default;

INDEX_TEMPLATE_ARGUMENTS
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator(BufferPoolManager *bpm, ReadPageGuard guard)
    : bpm_(bpm), guard_(std::move(guard)) {
  // Position at the first entry, or become an end iterator if the leaf is empty.
  offset_ = (guard_->template As<LeafPage>()->GetSize() > 0) ? 0 : -1;
}

INDEX_TEMPLATE_ARGUMENTS
auto IndexIterator<KeyType, ValueType, KeyComparator>::IsEnd() -> bool { return offset_ == -1; }

INDEX_TEMPLATE_ARGUMENTS
auto IndexIterator<KeyType, ValueType, KeyComparator>::operator*() -> std::pair<KeyType, ValueType> {
  auto leaf = guard_->template As<LeafPage>();
  return {leaf->KeyAt(offset_), leaf->ValueAt(offset_)};
}

INDEX_TEMPLATE_ARGUMENTS
auto IndexIterator<KeyType, ValueType, KeyComparator>::operator++() -> IndexIterator & {
  auto leaf = guard_->template As<LeafPage>();
  if (offset_ + 1 < leaf->GetSize()) {
    ++offset_;
    return *this;
  }
  // Move to the next leaf in the linked list.
  auto next = leaf->GetNextPageId();
  if (next == INVALID_PAGE_ID) {
    guard_.reset();
    offset_ = -1;
    return *this;
  }
  guard_ = bpm_->ReadPage(next);
  offset_ = (guard_->template As<LeafPage>()->GetSize() > 0) ? 0 : -1;
  return *this;
}

template class IndexIterator<GenericKey<4>, RID, GenericComparator<4>>;
template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>>;
template class IndexIterator<GenericKey<16>, RID, GenericComparator<16>>;
template class IndexIterator<GenericKey<32>, RID, GenericComparator<32>>;
template class IndexIterator<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bumblebee
