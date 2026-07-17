//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// disk_manager.h
//
// Identification: src/include/storage/disk/disk_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"
#include "common/macros.h"

namespace bumblebee {

/**
 * @brief A page-granular block device: the abstract backend the buffer pool reads and writes through.
 *
 * The buffer pool assigns page ids from a monotonically increasing counter; a concrete backend maps
 * a page id to a physical location lazily. That split is what keeps the interface extensible: a
 * single-file backend maps page id to a byte offset, a page-per-file backend to a path, and a cloud
 * backend to an object key — each only needs its own `page_id -> location` scheme.
 *
 * Every method reports success as a `bool` so the disk scheduler and buffer pool can *await and check*
 * an I/O (in particular, a dirty write-back) rather than assuming it succeeded.
 */
class DiskManager {
 public:
  DiskManager() = default;
  virtual ~DiskManager() = default;
  DISALLOW_COPY_AND_MOVE(DiskManager);

  /**
   * @brief Write a full page to the backing store.
   *
   * @param page_id The page to write.
   * @param page_data A `PAGE_SIZE`-byte buffer to write.
   * @return true on success. On an I/O error the method returns false and leaves no partial state
   *         (no half-updated mapping, no leaked allocation).
   */
  virtual auto WritePage(page_id_t page_id, const_data_ptr_t page_data) -> bool = 0;

  /**
   * @brief Read a full page from the backing store.
   *
   * Reading a page that was never written yields a zero-filled buffer and must not mutate any
   * allocation state — a read never allocates.
   *
   * @param page_id The page to read.
   * @param[out] page_data A `PAGE_SIZE`-byte output buffer.
   * @return true on success (including the zero-filled never-written case); false on an I/O error.
   */
  virtual auto ReadPage(page_id_t page_id, data_ptr_t page_data) -> bool = 0;

  /**
   * @brief Reclaim the space backing a page. Idempotent for pages that do not exist.
   *
   * @param page_id The page to delete.
   */
  virtual auto DeletePage(page_id_t page_id) -> void = 0;
};

}  // namespace bumblebee
