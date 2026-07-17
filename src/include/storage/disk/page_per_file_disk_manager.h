//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// page_per_file_disk_manager.h
//
// Identification: src/include/storage/disk/page_per_file_disk_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <filesystem>
#include <utility>

#include "common/exception.h"
#include "storage/disk/disk_manager.h"

namespace bumblebee {

/**
 * @brief DEFERRED: a disk manager that stores each page in its own file within a directory.
 *
 * This is a placeholder proving the `DiskManager` interface is extensible to a page-per-file layout
 * (`page_id -> directory/<page_id>.page`). It is not implemented this milestone; every method throws.
 */
class PagePerFileDiskManager : public DiskManager {
 public:
  explicit PagePerFileDiskManager(std::filesystem::path dir) : dir_(std::move(dir)) {}

  auto WritePage(page_id_t /*page_id*/, const_data_ptr_t /*page_data*/) -> bool override {
    throw NotImplementedException("PagePerFileDiskManager is not implemented yet");
  }

  auto ReadPage(page_id_t /*page_id*/, data_ptr_t /*page_data*/) -> bool override {
    throw NotImplementedException("PagePerFileDiskManager is not implemented yet");
  }

  auto DeletePage(page_id_t /*page_id*/) -> void override {
    throw NotImplementedException("PagePerFileDiskManager is not implemented yet");
  }

 private:
  std::filesystem::path dir_;
};

}  // namespace bumblebee
