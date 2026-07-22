//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_index.h
//
// Identification: src/include/storage/index/b_plus_tree_index.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "storage/buffer/buffer_pool_manager.h"
#include "storage/index/b_plus_tree.h"
#include "storage/index/generic_key.h"
#include "storage/index/index.h"
#include "storage/table/rid.h"

namespace bumblebee {

/**
 * @brief A B+ tree index over a fixed-size key.
 *
 * Wraps a `BPlusTree<GenericKey<KeySize>, RID, GenericComparator<KeySize>>`. The comparator reads the
 * key bytes through the metadata's key schema, so `KeySize` must cover the key schema's inlined width.
 */
template <size_t KeySize>
class BPlusTreeIndex : public Index {
  using KeyType = GenericKey<KeySize>;
  using ComparatorType = GenericComparator<KeySize>;
  using TreeType = BPlusTree<KeyType, RID, ComparatorType>;

 public:
  BPlusTreeIndex(std::unique_ptr<IndexMetadata> metadata, BufferPoolManager *bpm)
      : Index(std::move(metadata)),
        comparator_(&metadata_->GetKeySchema()),
        tree_(std::make_unique<TreeType>(metadata_->GetName(), bpm->NewPage(), bpm, comparator_)) {}

  /** @brief Open an EXISTING index over `header_page_id` (recovery): trust the on-disk B+ tree root. */
  BPlusTreeIndex(std::unique_ptr<IndexMetadata> metadata, BufferPoolManager *bpm, page_id_t header_page_id)
      : Index(std::move(metadata)),
        comparator_(&metadata_->GetKeySchema()),
        tree_(std::make_unique<TreeType>(typename TreeType::OpenExisting{}, metadata_->GetName(), header_page_id,
                                         bpm, comparator_)) {}

  auto GetHeaderPageId() const -> page_id_t override { return tree_->GetHeaderPageId(); }
  auto GetKeySize() const -> size_t override { return KeySize; }

  void FreeAllPages() override { tree_->FreeAllPages(); }

  void InsertEntry(const_data_ptr_t key, uint32_t key_len, RID rid) override {
    KeyType index_key;
    index_key.SetFromKey(key, key_len);
    tree_->Insert(index_key, rid);
  }

  void DeleteEntry(const_data_ptr_t key, uint32_t key_len, RID /*rid*/) override {
    KeyType index_key;
    index_key.SetFromKey(key, key_len);
    tree_->Remove(index_key);
  }

  void ScanKey(const_data_ptr_t key, uint32_t key_len, std::vector<RID> *result) override {
    KeyType index_key;
    index_key.SetFromKey(key, key_len);
    tree_->GetValue(index_key, result);
  }

 private:
  ComparatorType comparator_;
  std::unique_ptr<TreeType> tree_;
};

}  // namespace bumblebee
