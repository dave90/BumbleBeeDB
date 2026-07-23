//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_file_metadata_cache.h
//
// Identification: src/include/storage/parquet/parquet_file_metadata_cache.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <ctime>
#include <memory>

#include "parquet/parquet_types.h"

namespace bumblebee {

/** @brief An immutable parsed parquet footer, shared across readers of the same file. */
class ParquetFileMetadataCache {
 public:
  ParquetFileMetadataCache() : metadata_(nullptr), read_time_(0) {}
  ParquetFileMetadataCache(std::unique_ptr<format::FileMetaData> file_metadata, time_t r_time)
      : metadata_(std::move(file_metadata)), read_time_(r_time) {}

  std::unique_ptr<const format::FileMetaData> metadata_;
  time_t read_time_;
};

}  // namespace bumblebee
