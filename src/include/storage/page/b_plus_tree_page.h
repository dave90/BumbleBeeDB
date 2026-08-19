//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_page.h
//
// Identification: src/include/storage/page/b_plus_tree_page.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>

#include "storage/index/generic_key.h"

namespace bumblebee {

#define INDEX_TEMPLATE_ARGUMENTS template <typename KeyType, typename ValueType, typename KeyComparator>

/** The kind of B+ tree page. */
enum class IndexPageType { INVALID_INDEX_PAGE = 0, LEAF_PAGE, INTERNAL_PAGE };

/**
 * @brief The shared 12-byte header of every B+ tree page: page type, current size, and max size.
 *
 * Instances are always overlays reinterpreted onto a pinned frame, so all constructors are deleted.
 */
class BPlusTreePage {
 public:
  BPlusTreePage() = delete;
  BPlusTreePage(const BPlusTreePage &other) = delete;
  ~BPlusTreePage() = delete;

  auto IsLeafPage() const -> bool;
  void SetPageType(IndexPageType page_type);

  auto GetSize() const -> int;
  void SetSize(int size);
  void ChangeSizeBy(int amount);

  auto GetMaxSize() const -> int;
  void SetMaxSize(int max_size);
  auto GetMinSize() const -> int;

  /** @brief Upper-bound binary search over a page's keys: first index whose key is > `key`. */
  template <typename KeyType, typename KeyComparator, typename BPlusTreeKeyPage>
  static auto BisectRight(const BPlusTreeKeyPage &page, const KeyType &key, KeyComparator comparator,
                          int offset = 0) -> int {
    auto lo = offset;
    auto hi = page.GetSize();
    while (lo < hi) {
      auto mid = (lo + hi) / 2;
      if (comparator(page.KeyAt(mid), key) <= 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  /** @brief Shift `n` array elements from `src` to `dst`, in the safe direction. */
  template <typename T>
  static void ShiftBlocks(T *a, size_t dst, size_t src, size_t n) {
    if (dst < src) {
      for (size_t i = 0; i < n; ++i) {
        a[dst + i] = a[src + i];
      }
    } else if (dst > src) {
      for (size_t i = n; i > 0; --i) {
        a[dst + i - 1] = a[src + i - 1];
      }
    }
  }

 private:
  IndexPageType page_type_;
  int size_;
  int max_size_;
};

}  // namespace bumblebee
