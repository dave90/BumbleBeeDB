//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// thrift_tools.h
//
// Identification: src/include/storage/parquet/thrift_tools.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/allocator.h"
#include "storage/parquet/parquet_file.h"
#include "thrift/transport/TVirtualTransport.h"

namespace bumblebee {

/**
 * @brief Thrift transport reading from a ParquetFileHandle at a movable absolute location,
 * with an optional prefetched window (used for the footer, which is parsed in one pass).
 */
class ThriftFileTransport : public thrift::transport::TVirtualTransport<ThriftFileTransport> {
 public:
  ThriftFileTransport(Allocator &allocator, ParquetFileHandle &handle)
      : allocator_(allocator), handle_(handle), location_(0) {}

  auto read(uint8_t *buf, uint32_t len) -> uint32_t {  // NOLINT: thrift interface
    if (prefetched_data_ && location_ >= prefetch_location_ &&
        location_ + len < prefetch_location_ + prefetched_data_->GetSize()) {
      std::memcpy(buf, prefetched_data_->Get() + location_ - prefetch_location_, len);
    } else {
      handle_.Read(buf, len, location_);
    }
    location_ += len;
    return len;
  }

  /** @brief Read `[pos, pos+len)` into memory; subsequent reads inside the window skip IO. */
  void Prefetch(idx_t pos, idx_t len) {
    prefetch_location_ = pos;
    prefetched_data_ = allocator_.Allocate(len);
    handle_.Read(prefetched_data_->Get(), len, prefetch_location_);
  }

  void SetLocation(idx_t location) { location_ = location; }
  auto GetLocation() const -> idx_t { return location_; }
  auto GetSize() const -> idx_t { return handle_.FileSize(); }

 private:
  Allocator &allocator_;
  ParquetFileHandle &handle_;
  idx_t location_;

  alloc_data_ptr_t prefetched_data_;
  idx_t prefetch_location_{0};
};

}  // namespace bumblebee
