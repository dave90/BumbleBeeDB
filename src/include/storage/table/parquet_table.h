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
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "storage/table/table_storage.h"

namespace bumblebee {

/**
 * @brief DEFERRED: a read-only columnar table backed by a Parquet file.
 *
 * A placeholder proving `TableStorage` is extensible to a columnar backend. When implemented, its
 * `MakeScan` will read row groups natively into `DataChunk`s (a `TableScan::Next` that fills a chunk
 * without any row↔vector bridge); mutation is unsupported. Not implemented this milestone.
 */
class ParquetTable : public TableStorage {
 public:
  explicit ParquetTable(std::string path) : path_(std::move(path)) {}

  auto GetFormat() const -> StorageFormat override { return StorageFormat::PARQUET; }

  auto MakeScan(const std::vector<idx_t> & /*projection*/ = {}) -> std::unique_ptr<TableScan> override {
    throw NotImplementedException("ParquetTable scan is not implemented yet");
  }

  void Append(DataChunk & /*chunk*/, Vector & /*out_rids*/) override {
    throw NotImplementedException("ParquetTable is read-only");
  }
  void Update(Vector & /*row_ids*/, DataChunk & /*chunk*/) override {
    throw NotImplementedException("ParquetTable is read-only");
  }
  void Delete(Vector & /*row_ids*/, idx_t /*count*/) override {
    throw NotImplementedException("ParquetTable is read-only");
  }
  void Fetch(Vector & /*row_ids*/, idx_t /*count*/, DataChunk & /*out*/) override {
    throw NotImplementedException("ParquetTable is not implemented yet");
  }

 private:
  std::string path_;
};

}  // namespace bumblebee
