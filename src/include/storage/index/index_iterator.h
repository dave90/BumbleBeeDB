//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// index_iterator.h
//
// Identification: src/include/storage/index/index_iterator.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <utility>

#include "common/config.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/page_guard.h"

namespace bumblebee {

#define INDEXITERATOR_TYPE IndexIterator<KeyType, ValueType, KeyComparator>

/**
 * @brief A forward iterator over the leaves of a B+ tree, for range scans.
 *
 * Holds a read guard on the current leaf, walking the leaf linked list via `GetNextPageId`. An
 * end iterator holds no guard and reports `offset_ == -1`.
 */
INDEX_TEMPLATE_ARGUMENTS
class IndexIterator {
 public:
  using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;

  IndexIterator();
  IndexIterator(BufferPoolManager *bpm, ReadPageGuard guard);
  IndexIterator(IndexIterator &&) noexcept = default;
  auto operator=(IndexIterator &&) noexcept -> IndexIterator & = default;
  ~IndexIterator() = default;

  auto IsEnd() -> bool;
  auto operator*() -> std::pair<KeyType, ValueType>;
  auto operator++() -> IndexIterator &;

  auto operator==(const IndexIterator &itr) const -> bool {
    if (offset_ == -1 && itr.offset_ == -1) {
      return true;
    }
    if (!guard_.has_value() || !itr.guard_.has_value()) {
      return false;
    }
    return guard_->GetPageId() == itr.guard_->GetPageId() && offset_ == itr.offset_;
  }

  auto operator!=(const IndexIterator &itr) const -> bool { return !(*this == itr); }

 private:
  BufferPoolManager *bpm_{nullptr};
  std::optional<ReadPageGuard> guard_;
  int offset_{-1};
};

}  // namespace bumblebee
