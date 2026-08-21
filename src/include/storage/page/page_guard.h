//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// page_guard.h
//
// Identification: src/include/storage/page/page_guard.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <mutex>  // NOLINT

#include "common/config.h"
#include "storage/buffer/arc_replacer.h"
#include "storage/disk/disk_scheduler.h"

namespace bumblebee {

class BufferPoolManager;
class FrameHeader;

/**
 * @brief An RAII guard granting thread-safe, shared read access to a page.
 *
 * The buffer pool's page data is only ever reached through a page guard. A `ReadPageGuard` holds the
 * frame's reader latch in shared mode, so many readers may coexist but no writer can. Only the buffer
 * pool manager can construct a valid guard.
 */
class ReadPageGuard {
  friend class BufferPoolManager;

 public:
  ReadPageGuard() = default;

  ReadPageGuard(const ReadPageGuard &) = delete;
  auto operator=(const ReadPageGuard &) -> ReadPageGuard & = delete;
  ReadPageGuard(ReadPageGuard &&that) noexcept;
  auto operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard &;

  auto GetPageId() const -> page_id_t;
  auto GetData() const -> const_data_ptr_t;
  template <class T>
  auto As() const -> const T * {
    return reinterpret_cast<const T *>(GetData());
  }
  auto IsDirty() const -> bool;
  void Drop();
  ~ReadPageGuard();

 private:
  explicit ReadPageGuard(page_id_t page_id, std::shared_ptr<FrameHeader> frame, ArcReplacer *replacer,
                         std::mutex *bpm_latch, DiskScheduler *disk_scheduler);

  page_id_t page_id_{INVALID_PAGE_ID};
  std::shared_ptr<FrameHeader> frame_;
  // Non-owning: all three belong to the BufferPoolManager, which outlives every guard — a live
  // guard pins one of the pool's frames, so a guard outliving the pool is already a bug. Holding
  // shared_ptr copies here cost three atomic refcount pairs per page access for a lifetime the
  // pool guarantees anyway.
  ArcReplacer *replacer_{nullptr};
  std::mutex *bpm_latch_{nullptr};
  DiskScheduler *disk_scheduler_{nullptr};
  bool is_valid_{false};
};

/**
 * @brief An RAII guard granting thread-safe, exclusive write access to a page.
 *
 * A `WritePageGuard` holds the frame's reader/writer latch in exclusive mode, so it is the sole
 * accessor of the page — no other read or write guard on the same page can exist concurrently. Only
 * the buffer pool manager can construct a valid guard.
 */
class WritePageGuard {
  friend class BufferPoolManager;

 public:
  WritePageGuard() = default;

  WritePageGuard(const WritePageGuard &) = delete;
  auto operator=(const WritePageGuard &) -> WritePageGuard & = delete;
  WritePageGuard(WritePageGuard &&that) noexcept;
  auto operator=(WritePageGuard &&that) noexcept -> WritePageGuard &;

  auto GetPageId() const -> page_id_t;
  auto GetData() const -> const_data_ptr_t;
  template <class T>
  auto As() const -> const T * {
    return reinterpret_cast<const T *>(GetData());
  }
  auto GetDataMut() -> data_ptr_t;
  template <class T>
  auto AsMut() -> T * {
    return reinterpret_cast<T *>(GetDataMut());
  }
  auto IsDirty() const -> bool;
  /** @brief Flush this page to disk if dirty. Safe: the exclusive latch is already held. */
  void Flush();
  void Drop();
  ~WritePageGuard();

 private:
  explicit WritePageGuard(page_id_t page_id, std::shared_ptr<FrameHeader> frame, ArcReplacer *replacer,
                          std::mutex *bpm_latch, DiskScheduler *disk_scheduler);
  explicit WritePageGuard(std::try_to_lock_t, page_id_t page_id, std::shared_ptr<FrameHeader> frame,
                          ArcReplacer *replacer, std::mutex *bpm_latch, DiskScheduler *disk_scheduler);

  page_id_t page_id_{INVALID_PAGE_ID};
  std::shared_ptr<FrameHeader> frame_;
  // Non-owning; see ReadPageGuard for the lifetime argument.
  ArcReplacer *replacer_{nullptr};
  std::mutex *bpm_latch_{nullptr};
  DiskScheduler *disk_scheduler_{nullptr};
  bool is_valid_{false};
};

}  // namespace bumblebee
