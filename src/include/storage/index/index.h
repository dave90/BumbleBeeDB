//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// index.h
//
// Identification: src/include/storage/index/index.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/macros.h"
#include "storage/table/rid.h"

namespace bumblebee {

/**
 * @brief Describes an index: its name, the schema of its key, and which table columns form the key.
 */
class IndexMetadata {
 public:
  IndexMetadata(std::string name, std::string table_name, Schema key_schema, std::vector<uint32_t> key_attrs)
      : name_(std::move(name)),
        table_name_(std::move(table_name)),
        key_schema_(std::move(key_schema)),
        key_attrs_(std::move(key_attrs)) {}

  auto GetName() const -> const std::string & { return name_; }

  auto GetKeySchema() const -> const Schema & { return key_schema_; }
  /** @return The index of each key column within the base table's schema. */
  auto GetKeyAttrs() const -> const std::vector<uint32_t> & { return key_attrs_; }

 private:
  std::string name_;
  std::string table_name_;
  Schema key_schema_;
  std::vector<uint32_t> key_attrs_;
};

/**
 * @brief The abstract interface every index implements.
 *
 * Keys are passed as raw bytes laid out at the key schema's column offsets (the same layout the
 * concrete comparator reads).
 */
class Index {
 public:
  explicit Index(std::unique_ptr<IndexMetadata> metadata) : metadata_(std::move(metadata)) {}
  virtual ~Index() = default;
  Index(const Index &) = delete;
  auto operator=(const Index &) -> Index & = delete;
  Index(Index &&) = delete;
  auto operator=(Index &&) -> Index & = delete;

  auto GetMetadata() const -> IndexMetadata * { return metadata_.get(); }

  /**
   * @brief Atomically insert a unique (key, rid) entry.
   * @return True when this call published the key; false when the key already exists.
   */
  [[nodiscard]] virtual auto InsertEntry(const_data_ptr_t key, uint32_t key_len, RID rid) -> bool = 0;

  /** @brief Delete the entry for `key`. */
  virtual void DeleteEntry(const_data_ptr_t key, uint32_t key_len, RID rid) = 0;

  /** @brief Append the rids matching `key` to `result`. */
  virtual void ScanKey(const_data_ptr_t key, uint32_t key_len, std::vector<RID> *result) = 0;

  /** @return The on-disk page that anchors this index (e.g. a B+ tree header page), for persistence. */
  virtual auto GetHeaderPageId() const -> page_id_t { return INVALID_PAGE_ID; }

  /** @return The byte width of this index's key slot (the concrete `KeySize`), for persistence. */
  virtual auto GetKeySize() const -> size_t { return 0; }

  /**
   * @brief Return every page this index owns to the buffer pool's free list (DROP TABLE / DROP INDEX).
   *
   * After this call the index must not be used again. The default is a no-op for index kinds that own
   * no pages of their own.
   */
  virtual void FreeAllPages() {}

 protected:
  std::unique_ptr<IndexMetadata> metadata_;
};

}  // namespace bumblebee
