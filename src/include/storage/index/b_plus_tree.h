//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree.h
//
// Identification: src/include/storage/index/b_plus_tree.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/page_guard.h"

namespace bumblebee {

/**
 * @brief Tracks the guards held during a latch-crabbing insert or delete.
 *
 * `header_page_` is the write guard on the header page (held while the root might change);
 * `write_set_` is the front-to-back chain of guards from the root down to the working node.
 */
class Context {
 public:
  std::optional<WritePageGuard> header_page_{std::nullopt};
  page_id_t root_page_id_{INVALID_PAGE_ID};
  std::deque<WritePageGuard> write_set_;
  std::deque<ReadPageGuard> read_set_;

  auto IsRootPage(page_id_t page_id) -> bool { return page_id == root_page_id_; }
};

#define BPLUSTREE_TYPE BPlusTree<KeyType, ValueType, KeyComparator>

/**
 * @brief A canonical B+ tree over unique keys, with pessimistic latch-crabbing via a header page.
 *
 * Supports point lookup, insert, remove (with split / redistribute / merge / root collapse), and a
 * forward range iterator.
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTree {
  using InternalPage = BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>;
  using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;
  using IndexIteratorType = IndexIterator<KeyType, ValueType, KeyComparator>;

  struct InsertInternalResult {
    std::optional<KeyType> new_key_;
    std::optional<page_id_t> new_page_id_;
    bool inserted_{false};
  };

 public:
  /** Tag to select the "open an existing on-disk tree" constructor (trusts the persisted root). */
  struct OpenExisting {};

  explicit BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                     const KeyComparator &comparator, int leaf_max_size = LEAF_PAGE_SLOT_CNT,
                     int internal_max_size = INTERNAL_PAGE_SLOT_CNT);

  /** @brief Open an EXISTING tree over `header_page_id`: do NOT reset the on-disk root. */
  BPlusTree(OpenExisting, std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
            const KeyComparator &comparator, int leaf_max_size = LEAF_PAGE_SLOT_CNT,
            int internal_max_size = INTERNAL_PAGE_SLOT_CNT);

  auto IsEmpty() const -> bool;
  auto Insert(const KeyType &key, const ValueType &value) -> bool;
  void Remove(const KeyType &key);
  auto GetValue(const KeyType &key, std::vector<ValueType> *result) -> bool;
  auto GetRootPageId() -> page_id_t;
  auto GetHeaderPageId() const -> page_id_t { return header_page_id_; }

  auto Begin() -> IndexIteratorType;
  auto Begin(const KeyType &key) -> IndexIteratorType;
  auto End() -> IndexIteratorType;

 private:
  auto RecursiveInsert(Context &ctx, const KeyType &key, const ValueType &value) -> InsertInternalResult;
  auto InitBPlusTree(const KeyType &key, const ValueType &value) -> page_id_t;
  auto RecursiveInsertLeaf(Context &ctx, const KeyType &key, const ValueType &value) -> InsertInternalResult;
  /** @brief Fast path: read-crab to the leaf, take one write latch; false ⇒ retry pessimistically. */
  auto TryOptimisticInsert(const KeyType &key, const ValueType &value, bool &insert_result) -> bool;

  auto RecursiveDeleteLeaf(Context &ctx, const KeyType &key) -> bool;
  auto GetBestSiblingForMerge(InternalPage &parent_page, int pidx, int &sib_idx) -> WritePageGuard;
  auto RecursiveDelete(Context &ctx, const KeyType &key) -> bool;
  /** @brief Fast path: read-crab to the leaf, take one write latch; false ⇒ retry pessimistically. */
  auto TryOptimisticDelete(const KeyType &key) -> bool;

  BufferPoolManager *bpm_;
  std::string index_name_;
  KeyComparator comparator_;
  int leaf_max_size_;
  int internal_max_size_;
  page_id_t header_page_id_;
};

}  // namespace bumblebee
