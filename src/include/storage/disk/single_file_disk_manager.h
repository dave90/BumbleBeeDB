//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// single_file_disk_manager.h
//
// Identification: src/include/storage/disk/single_file_disk_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>  // NOLINT
#include <string>

#include "storage/disk/disk_manager.h"

namespace bumblebee {

/**
 * @brief A disk manager that stores every page in a single file at a fixed, self-describing offset.
 *
 * Page N lives at byte offset `N * PAGE_SIZE` — the offset is a pure function of the id, so nothing
 * needs to be persisted to locate a page and the file survives a restart with no in-memory mapping to
 * rebuild. A never-written (or deleted) page reads back as zeros; deleting a page zero-fills its slot.
 * Page-id reuse (reclaiming a deleted slot) is the buffer pool's responsibility via its free list.
 * There is no log / write-ahead subsystem here — crash recovery is a later milestone.
 */
class SingleFileDiskManager : public DiskManager {
 public:
  explicit SingleFileDiskManager(const std::filesystem::path &db_file);

  ~SingleFileDiskManager() override;

  auto WritePage(page_id_t page_id, const_data_ptr_t page_data) -> bool override;

  auto ReadPage(page_id_t page_id, data_ptr_t page_data) -> bool override;

  auto DeletePage(page_id_t page_id) -> void override;

  /** @brief Close the file. Safe to call more than once. */
  void ShutDown();

  /** @return The number of successful page writes. */
  auto GetNumWrites() const -> int { return num_writes_; }

  /** @return The number of page deletions. */
  auto GetNumDeletes() const -> int { return num_deletes_; }

  /** @return The size of the database file in bytes, or 0 if it cannot be stat'd. */
  auto GetDbFileSize() -> size_t;

 private:
  /** @brief Ensure the file is large enough to hold `page_id` (grow, doubling capacity, if needed). */
  void EnsureCapacity(page_id_t page_id);

  /**
   * @brief Stat the file's size in bytes.
   *
   * Returns a 64-bit size: the database file routinely passes 2 GiB once out-of-core
   * operators spill into it, and a 32-bit result would wrap negative there — which
   * ReadPage reads as an I/O error, failing every page read past that point.
   *
   * @return int64_t The size in bytes, or -1 if the file cannot be stat'd.
   */
  static auto GetFileSize(const std::string &file_name) -> int64_t;

  int num_writes_{0};
  int num_deletes_{0};

  /** The number of pages the file is currently sized for. Doubles on demand; never shrinks. */
  size_t page_capacity_{DEFAULT_DB_IO_SIZE};

  std::fstream db_io_;
  std::filesystem::path db_file_name_;

  /** Serializes all file access. */
  std::mutex db_io_latch_;
};

}  // namespace bumblebee
