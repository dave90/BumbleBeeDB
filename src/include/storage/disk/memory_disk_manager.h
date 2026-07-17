//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// memory_disk_manager.h
//
// Identification: src/include/storage/disk/memory_disk_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstring>
#include <vector>

#include "storage/disk/disk_manager.h"

namespace bumblebee {

/**
 * @brief An in-memory disk manager for tests: a fixed, contiguous buffer of `capacity` pages.
 *
 * Page ids index directly into the buffer, so a page id must be in `[0, capacity)`. Reads and writes
 * bounds-check the id before touching memory — an out-of-range page never reads or writes past the
 * buffer (it zero-fills / fails instead).
 */
class MemoryDiskManager : public DiskManager {
 public:
  explicit MemoryDiskManager(size_t capacity) : capacity_(capacity), memory_(capacity * PAGE_SIZE, 0) {}

  ~MemoryDiskManager() override = default;

  auto WritePage(page_id_t page_id, const_data_ptr_t page_data) -> bool override {
    if (page_id < 0 || static_cast<size_t>(page_id) >= capacity_) {
      return false;
    }
    std::memcpy(memory_.data() + static_cast<size_t>(page_id) * PAGE_SIZE, page_data, PAGE_SIZE);
    return true;
  }

  auto ReadPage(page_id_t page_id, data_ptr_t page_data) -> bool override {
    if (page_id < 0 || static_cast<size_t>(page_id) >= capacity_) {
      // Out of range: never read past the buffer. Report failure with a defined output.
      std::memset(page_data, 0, PAGE_SIZE);
      return false;
    }
    std::memcpy(page_data, memory_.data() + static_cast<size_t>(page_id) * PAGE_SIZE, PAGE_SIZE);
    return true;
  }

  auto DeletePage(page_id_t /*page_id*/) -> void override {}

  /** @return The number of pages this manager can hold. */
  auto GetCapacity() const -> size_t { return capacity_; }

 private:
  size_t capacity_;
  std::vector<char> memory_;
};

}  // namespace bumblebee
