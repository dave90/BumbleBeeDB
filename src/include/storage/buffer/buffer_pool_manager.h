//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// buffer_pool_manager.h
//
// Identification: src/include/storage/buffer/buffer_pool_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <condition_variable>  // NOLINT
#include <list>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "storage/buffer/arc_replacer.h"
#include "storage/disk/disk_scheduler.h"
#include "storage/page/page_guard.h"

namespace bumblebee {

class BufferPoolManager;
class ReadPageGuard;
class WritePageGuard;

/**
 * @brief Metadata for one frame of the buffer pool: the latch, pin count, dirty flag, and data.
 *
 * Each frame allocates its own `data_` buffer (rather than carving from one big block) so that
 * AddressSanitizer catches any out-of-bounds page access during development.
 */
class FrameHeader {
  friend class BufferPoolManager;
  friend class ReadPageGuard;
  friend class WritePageGuard;

 public:
  explicit FrameHeader(frame_id_t frame_id);

 private:
  auto GetData() const -> const_data_ptr_t;
  auto GetDataMut() -> data_ptr_t;
  void Reset();

  /** The index of this frame in the buffer pool. */
  const frame_id_t frame_id_;

  /** The reader/writer latch — a read guard holds it shared, a write guard exclusive. */
  std::shared_mutex rwlatch_;

  /** How many guards currently pin this frame in memory. */
  std::atomic<size_t> pin_count_;

  /** Whether the page has been modified since it was read in. */
  bool is_dirty_;

  /** The page's data: a fixed PAGE_SIZE heap buffer (its own allocation so ASan flags OOB access). */
  std::unique_ptr<data_t[]> data_;

  /** The page currently in this frame, or nullopt when free. */
  std::optional<page_id_t> page_id_;
};

/**
 * @brief Moves pages between memory frames and the disk backend, caching hot pages.
 *
 * The buffer pool hands out `page_id`s from a monotonic counter (`NewPage`) and materializes a page
 * lazily the first time it is read or written. Pages are accessed exclusively through page guards.
 */
class BufferPoolManager {
 public:
  BufferPoolManager(size_t num_frames, DiskManager *disk_manager, page_id_t initial_next_page_id = 0);
  ~BufferPoolManager();

  DISALLOW_COPY_AND_MOVE(BufferPoolManager);

  /** @return The number of frames in the pool. */
  auto Size() const -> size_t;

  /** @return A freshly allocated page id: a reclaimed one from the free list, else the next new id. */
  auto NewPage() -> page_id_t;

  /** @brief Delete a page from memory and disk, reclaiming its id for reuse. False if it is pinned. */
  auto DeletePage(page_id_t page_id) -> bool;

  // --- Allocator persistence (for the durable Database owner) ---------------------------------

  /** @return The next-new-id high-water mark (ids below this were handed out at some point). */
  auto GetNextPageId() const -> page_id_t { return next_page_id_.load(); }

  /** @return A snapshot of the free list of reclaimed, reusable page ids. */
  auto GetFreePages() -> std::vector<page_id_t>;

  /** @brief Restore the free list of reusable page ids (on reopen). */
  void SetFreePages(std::vector<page_id_t> free_pages);

  /**
   * @brief Allocate a fresh page id by bumping the high-water mark only, bypassing the free list.
   *
   * Used for permanent, self-managed pages (the catalog chain) that must not consume reclaimable ids
   * — and, because it changes only the fixed-width high-water mark, does not perturb the persisted
   * free list, so a record that embeds the allocator state keeps a stable size when it grows.
   */
  auto AllocateRawPageId() -> page_id_t { return next_page_id_.fetch_add(1); }

  /** @brief Acquire an exclusive write guard, or nullopt if the pool is out of frames. */
  auto CheckedWritePage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> std::optional<WritePageGuard>;

  /** @brief Acquire a shared read guard, or nullopt if the pool is out of frames. */
  auto CheckedReadPage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> std::optional<ReadPageGuard>;

  /** @brief Acquire a write guard, aborting the process if the pool is out of frames. */
  auto WritePage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> WritePageGuard;

  /** @brief Acquire a read guard, aborting the process if the pool is out of frames. */
  auto ReadPage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> ReadPageGuard;

  /** @brief Flush a page to disk under its latch. Returns false if the page is not in memory. */
  auto FlushPage(page_id_t page_id) -> bool;

  /** @brief Flush every in-memory page to disk. */
  void FlushAllPages();

  /** @brief The pin count of a page, or nullopt if it is not in memory. */
  auto GetPinCount(page_id_t page_id) -> std::optional<size_t>;

 private:
  /** @brief Bring a page into a frame, pin it, and return it (nullptr on out-of-memory). */
  auto CheckedPage(page_id_t page_id, AccessType access_type) -> std::shared_ptr<FrameHeader>;

  /**
   * @brief Free one frame so a caller can reuse it, flushing a dirty victim to disk *outside* the pool
   * latch (`lk` is passed in and released around the I/O).
   *
   * The victim is detached from the page table and registered in `flushing_` before the lock is
   * dropped, so a concurrent fetch of that same page waits on `flush_cv_` rather than racing a read
   * against the in-flight write. @return true if a frame was freed; false on out-of-memory / I/O error.
   */
  auto EvictOneLocked(std::unique_lock<std::mutex> &lk) -> bool;

  /** @return The page ids currently resident, snapshotted under the pool latch. */
  auto GetAllPages() -> std::vector<page_id_t>;

  const size_t num_frames_;
  std::atomic<page_id_t> next_page_id_;
  /** Guards `free_pages_`, the list of reclaimed page ids `NewPage` reuses before `next_page_id_`. */
  std::mutex alloc_latch_;
  std::vector<page_id_t> free_pages_;

  /** The pool latch: guards `page_table_`, `free_frames_`, `replacer_`, and `flushing_`. */
  std::shared_ptr<std::mutex> latch_;
  std::vector<std::shared_ptr<FrameHeader>> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> free_frames_;
  std::shared_ptr<ArcReplacer> replacer_;
  /** Pages with an eviction write-back in flight; a fetch of one waits on `flush_cv_`. */
  std::unordered_set<page_id_t> flushing_;
  std::condition_variable flush_cv_;

  std::shared_ptr<DiskScheduler> disk_scheduler_;
};

}  // namespace bumblebee
