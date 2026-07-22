//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database.h
//
// Identification: src/include/database.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/byte_buffer.h"
#include "common/config.h"
#include "common/macros.h"
#include "concurrency/transaction_manager.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/single_file_disk_manager.h"

namespace bumblebee {

/**
 * @brief A durable database: owns the disk manager, buffer pool, and catalog over a single file.
 *
 * Opening reconstructs the in-memory catalog (tables, indexes) and the page allocator from a catalog
 * record on page 0; closing serializes them back and flushes every page. A never-before-seen file is
 * initialized fresh (page 0 reserved for the catalog). There is NO write-ahead log yet, so durability
 * holds only across a clean `Close()` — crash recovery is a future milestone.
 */
class Database {
 public:
  explicit Database(const std::filesystem::path &db_file, size_t num_frames = BUFFER_POOL_SIZE);
  ~Database();

  DISALLOW_COPY_AND_MOVE(Database);

  auto GetCatalog() -> Catalog & { return *catalog_; }
  auto GetBufferPool() -> BufferPoolManager & { return *bpm_; }
  auto GetTransactionManager() -> TransactionManager & { return *txn_manager_; }

  /** @brief Persist the catalog + allocator state and flush all pages to disk. Idempotent. */
  void Close();

 private:
  /** @brief Serialize the current catalog + allocator state into a byte record. */
  void SerializeCatalog(ByteWriter &w);

  /** @brief Reconstruct catalog + allocator from a record; constructs `bpm_` and `catalog_`. */
  void LoadCatalog(ByteReader &r);

  /**
   * @brief Read the catalog record from the on-disk page chain starting at page 0.
   * @return true and fills `record` if page 0 holds a valid catalog; false for a fresh (no-magic) file.
   *         Populates `catalog_pages_` with every page in the chain (so they are reused / not leaked).
   */
  auto ReadCatalogChain(std::vector<char> &record) -> bool;

  /** @brief Write `record` across `catalog_pages_`, growing the chain (raw-allocated pages) if needed. */
  void WriteCatalogChain(const std::vector<char> &record);

  /** @brief Reopen one index at its persisted `KeySize` (a runtime dispatch over supported sizes). */
  void LoadIndexDispatch(size_t key_size, index_oid_t oid, const std::string &name, const std::string &table_name,
                         const std::vector<uint32_t> &key_attrs, const Schema &key_schema, page_id_t header_page_id);

  static constexpr uint32_t kMagic = 0xB0BDB001;    // identifies a catalog root page
  static constexpr uint32_t kVersion = 3;           // catalog record format (v2: +last_commit_ts; v3: +PK/_id)
  static constexpr page_id_t kCatalogRootPage = 0;  // page 0 is reserved for the catalog chain head
  // Page 0 reserves magic(4) + total-length(4) + next(4); an overflow page reserves only next(4).
  static constexpr size_t kRootPayload = PAGE_SIZE - 12;
  static constexpr size_t kChainPayload = PAGE_SIZE - 4;

  std::unique_ptr<SingleFileDiskManager> dm_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<TransactionManager> txn_manager_;
  /** The page chain holding the catalog record (page 0 first). Grows as the catalog grows; never shrinks. */
  std::vector<page_id_t> catalog_pages_;
  size_t num_frames_;
  /** Commit-ts high-water mark read from the catalog record; seeded into `txn_manager_` after recovery. */
  timestamp_t recovered_commit_ts_{0};
  bool closed_{false};
};

}  // namespace bumblebee
