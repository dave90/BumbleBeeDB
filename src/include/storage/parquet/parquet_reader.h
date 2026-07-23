//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_reader.h
//
// Identification: src/include/storage/parquet/parquet_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/allocator.h"
#include "storage/parquet/column_reader.h"
#include "storage/parquet/parquet_file.h"
#include "storage/parquet/parquet_file_metadata_cache.h"
#include "parquet/parquet_types.h"
#include "thrift/protocol/TProtocol.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/** Column-id sentinel for "skip this output column" (no file column behind it). */
const idx_t COLUMN_IDENTIFIER_ROW_ID = static_cast<idx_t>(-1);

/**
 * @brief The process-wide allocator for parquet page/dictionary buffers.
 *
 * The default Allocator is a stateless malloc/free indirection, and a process-wide instance can
 * never be outlived by an AllocatedData — which matters because scan-local reader state (and the
 * zero-copy page buffers referenced by emitted vectors) can outlive the operator state that
 * opened the reader.
 */
auto GlobalParquetAllocator() -> Allocator &;

/** Per-scan cursor state; one per concurrent scanner of the same ParquetReader. */
struct ParquetReaderScanState {
  std::vector<idx_t> group_idx_list_;  // row groups to read
  int64_t current_group_;
  std::vector<idx_t> column_ids_;  // file column index per output column
  idx_t group_offset_;             // row offset inside the group
  std::unique_ptr<ParquetFileHandle> file_handle_;
  std::unique_ptr<ColumnReader> root_reader_;
  std::unique_ptr<TProtocol> thrift_file_proto_;

  bool finished_;

  ResizeableBuffer define_buf_;
  ResizeableBuffer repeat_buf_;
};

/**
 * @brief Reads one parquet file: parses/caches the footer, derives the schema as LogicalTypes,
 * and decodes row groups into DataChunks through per-column readers.
 */
class ParquetReader {
 public:
  /**
   * @brief Open `file_name` and parse its footer.
   *
   * @param allocator Allocator for page/dictionary buffers.
   * @param file_name Path of the parquet file.
   */
  ParquetReader(Allocator &allocator, std::string file_name);
  ~ParquetReader();

  Allocator &allocator_;
  std::string file_name_;
  std::vector<LogicalType> return_types_;  // per-column logical type
  std::vector<std::string> names_;         // per-column name
  std::shared_ptr<ParquetFileMetadataCache> metadata_;

  /**
   * @brief Prepare a scan cursor over `groups_to_read`, producing the file columns listed in
   * `column_ids` (COLUMN_IDENTIFIER_ROW_ID entries are left untouched in the output chunk).
   */
  void InitializeScan(ParquetReaderScanState &state, std::vector<idx_t> column_ids,
                      std::vector<idx_t> groups_to_read);

  /** @brief Decode the next chunk (up to STANDARD_VECTOR_SIZE rows) into `output`. Sets an empty
   * cardinality when the scan is exhausted. */
  void Scan(ParquetReaderScanState &state, DataChunk &output);

  /** @return Total rows in the file. */
  auto NumRows() -> idx_t;

  /** @return Number of row groups in the file. */
  auto NumRowGroups() -> idx_t;

  /** @return The parsed footer. */
  auto GetFileMetadata() -> const format::FileMetaData *;

 private:
  void InitializeSchema();
  auto ScanInternal(ParquetReaderScanState &state, DataChunk &output) -> bool;
  auto CreateReader(const format::FileMetaData *file_meta_data) -> std::unique_ptr<ColumnReader>;
  auto CreateReaderRecursive(const format::FileMetaData *file_meta_data, idx_t depth, idx_t max_define,
                             idx_t max_repeat, idx_t &next_schema_idx, idx_t &next_file_idx)
      -> std::unique_ptr<ColumnReader>;
  auto GetGroup(ParquetReaderScanState &state) -> const format::RowGroup &;
  void PrepareRowGroupBuffer(ParquetReaderScanState &state, idx_t out_col_idx);
  auto DeriveLogicalType(const format::SchemaElement &s_ele) -> LogicalType;

  std::unique_ptr<ParquetFileHandle> file_handle_;
};

using parquet_reader_ptr_t = std::unique_ptr<ParquetReader>;

}  // namespace bumblebee
