//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_writer.h
//
// Identification: src/include/storage/parquet/parquet_writer.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/parquet/parquet_file.h"
#include "parquet/parquet_types.h"
#include "thrift/protocol/TProtocol.h"
#include "type/logical_type.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

/**
 * @brief Writes one parquet file: each Flush() call emits one row group (PLAIN-encoded pages,
 * optionally compressed); Finalize() writes the footer and syncs the file.
 */
class ParquetWriter {
 public:
  /**
   * @param file_name Path of the file to create (truncated if present).
   * @param types Column logical types.
   * @param names Column names.
   * @param codec Page compression codec.
   */
  ParquetWriter(std::string file_name, std::vector<LogicalType> types, std::vector<std::string> names,
                format::CompressionCodec::type codec);

  /** @brief Write `buffer` as one row group. Thread-safe (serialized internally). */
  void Flush(ChunkCollection &buffer);

  /** @brief Write the footer, sync, and close the file. */
  void Finalize();

 private:
  std::string file_name_;
  std::vector<LogicalType> sql_types_;
  std::vector<std::string> column_names_;
  format::CompressionCodec::type codec_;

  std::unique_ptr<BufferedFileWriter> writer_;
  std::shared_ptr<thrift::protocol::TProtocol> protocol_;
  format::FileMetaData file_meta_data_;
  std::mutex lock_;
};

using parquet_writer_ptr_t = std::unique_ptr<ParquetWriter>;

}  // namespace bumblebee
