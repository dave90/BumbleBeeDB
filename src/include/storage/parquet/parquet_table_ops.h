//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_table_ops.h
//
// Identification: src/include/storage/parquet/parquet_table_ops.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "catalog/schema.h"
#include "storage/parquet/parquet_manifest.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

class ParquetTable;

/**
 * @brief RAII writer lock of an external parquet table: at most one writer per table, losers fail
 * immediately (no waiting).
 *
 * Two layers: the in-process `ParquetTable` mutex (the primary mechanism — the engine is a single
 * process), plus a `_bbdb_lock` file taken O_EXCL to defend against a second shell instance on the
 * same folder. A lock file older than STALE_LOCK_SECONDS is treated as leaked by a crash and
 * replaced. Acquired BEFORE the manifest the write is based on is read, so basing a rewrite on a
 * stale manifest (a lost update) is structurally impossible.
 */
class ExternalWriteGuard {
 public:
  static constexpr int STALE_LOCK_SECONDS = 600;

  /** @brief Acquire both locks or throw ExecutionException (concurrent writer). */
  explicit ExternalWriteGuard(ParquetTable &table, const std::string &table_name);
  ~ExternalWriteGuard();

  ExternalWriteGuard(const ExternalWriteGuard &) = delete;
  auto operator=(const ExternalWriteGuard &) -> ExternalWriteGuard & = delete;

 private:
  ParquetTable &table_;
  std::string lock_path_;
  bool process_locked_{false};
  bool file_locked_{false};
};

/** @return The schema's column types, in order (the writer wants a plain type vector). */
auto SchemaLogicalTypes(const Schema &schema) -> std::vector<LogicalType>;

/** @return A fresh unique part-file name for manifest version `version`. */
auto GeneratePartFileName(int64_t version) -> std::string;

/**
 * @brief Write `rows` as one parquet part file under `dir` and return its manifest entry.
 * Row groups are cut every ~64k rows so a rewrite of a big table never buffers it whole.
 */
auto WritePartFile(const std::string &dir, const std::string &file_name, const Schema &schema,
                   ChunkCollection &rows) -> ManifestEntry;

/**
 * @brief Streaming part-file writer for rewrites: chunks are appended and flushed as row groups
 * every ~64k rows, so the peak memory is one row group regardless of table size.
 */
class PartFileWriter {
 public:
  static constexpr idx_t ROWS_PER_ROW_GROUP = 64 * 1024;

  PartFileWriter(std::string dir, std::string file_name, const Schema &schema);
  ~PartFileWriter();

  /** @brief Append a chunk (deep-copied into the pending row group). */
  void Append(DataChunk &chunk);

  /** @brief Flush the pending rows and close the file. @return The file's manifest entry. */
  auto Finish() -> ManifestEntry;

  /** @return Rows appended so far. */
  auto RowCount() const -> idx_t { return total_rows_; }

 private:
  void FlushGroup();

  std::string dir_;
  std::string file_name_;
  std::vector<LogicalType> types_;
  std::vector<std::string> names_;
  std::unique_ptr<class ParquetWriter> writer_;
  ChunkCollection pending_;
  idx_t total_rows_{0};
  bool finished_{false};
};

}  // namespace bumblebee
