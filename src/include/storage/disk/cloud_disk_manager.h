//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// cloud_disk_manager.h
//
// Identification: src/include/storage/disk/cloud_disk_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>

#include "common/exception.h"
#include "storage/disk/disk_manager.h"

namespace bumblebee {

/**
 * @brief DEFERRED: a disk manager that stores each page as an object in a cloud object store.
 *
 * This is a placeholder proving the `DiskManager` interface is extensible to a cloud backend
 * (`page_id -> bucket/<prefix>/<page_id>`). It is not implemented this milestone; every method throws.
 * Because the disk scheduler already runs I/O on a background thread, an async cloud backend drops in
 * behind this same interface without changing the buffer pool.
 */
class CloudDiskManager : public DiskManager {
 public:
  CloudDiskManager(std::string bucket, std::string prefix)
      : bucket_(std::move(bucket)), prefix_(std::move(prefix)) {}

  auto WritePage(page_id_t /*page_id*/, const_data_ptr_t /*page_data*/) -> bool override {
    throw NotImplementedException("CloudDiskManager is not implemented yet");
  }

  auto ReadPage(page_id_t /*page_id*/, data_ptr_t /*page_data*/) -> bool override {
    throw NotImplementedException("CloudDiskManager is not implemented yet");
  }

  auto DeletePage(page_id_t /*page_id*/) -> void override {
    throw NotImplementedException("CloudDiskManager is not implemented yet");
  }

 private:
  std::string bucket_;
  std::string prefix_;
};

}  // namespace bumblebee
