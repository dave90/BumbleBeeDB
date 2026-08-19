//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_table.h
//
// Identification: src/include/storage/table/parquet_table.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "catalog/schema.h"
#include "common/exception.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/table/table_storage.h"

namespace bumblebee {

/**
 * @brief External table backed by parquet files in a user-provided folder.
 *
 * Data files are governed by the manifest protocol (see parquet/parquet_manifest.h): a scan reads
 * the newest manifest once (statement-level snapshot); writers rewrite part files copy-on-write
 * and commit by an atomic manifest swap. There is no MVCC and no transactionality — the manifest
 * swap is the commit point, and one writer at a time is enforced by `write_lock_` (a concurrent
 * writer fails immediately rather than waiting).
 *
 * The rid-oriented TableStorage mutation interface does not apply to copy-on-write storage; the
 * external write operators use the manifest/writer API directly, so those methods throw.
 */
class ParquetTable : public TableStorage {
 public:
  ParquetTable(std::string path, std::shared_ptr<Schema> schema)
      : path_(std::move(path)), schema_(std::move(schema)) {}

  auto GetFormat() const -> StorageFormat override { return StorageFormat::PARQUET; }

  /** @brief Row count from the newest manifest (sum of per-file counts); 0 when no manifest exists. */
  auto EstimatedRowCount() const -> idx_t override {
    auto manifest = ParquetManifestIO::ReadLatest(path_);
    return manifest.has_value() ? manifest->TotalRows() : 0;
  }

  /** @return The folder holding the table's data files. */
  auto GetPath() const -> const std::string & { return path_; }

  /** @return The table schema. */
  auto GetSchema() const -> const std::shared_ptr<Schema> & { return schema_; }

  /** @brief Fail-fast writer lock: at most one writer per table, losers throw immediately. */
  [[nodiscard]] auto TryLockForWrite() -> bool { return write_lock_.try_lock(); }
  void UnlockWrite() { write_lock_.unlock(); }

  auto MakeScan(const std::vector<idx_t> & /*projection*/ = {}) -> std::unique_ptr<TableScan> override {
    throw NotImplementedException("ParquetTable scan is not implemented yet");
  }

  void Append(DataChunk & /*chunk*/, Vector & /*out_rids*/) override {
    throw NotImplementedException("external parquet tables are written through the manifest protocol");
  }
  void Update(Vector & /*row_ids*/, DataChunk & /*chunk*/) override {
    throw NotImplementedException("external parquet tables are written through the manifest protocol");
  }
  void Delete(Vector & /*row_ids*/, idx_t /*count*/) override {
    throw NotImplementedException("external parquet tables are written through the manifest protocol");
  }
  void Fetch(Vector & /*row_ids*/, idx_t /*count*/, DataChunk & /*out*/) override {
    throw NotImplementedException("ParquetTable fetch is not implemented yet");
  }

 private:
  std::string path_;
  std::shared_ptr<Schema> schema_;
  std::mutex write_lock_;
};

}  // namespace bumblebee
