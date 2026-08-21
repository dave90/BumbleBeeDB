//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// buffer_pool_manager.cpp
//
// Identification: src/storage/buffer/buffer_pool_manager.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/buffer/buffer_pool_manager.h"

#include <cstdio>
#include <future>  // NOLINT
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <utility>
#include <vector>

#include "common/macros.h"

namespace bumblebee {

FrameHeader::FrameHeader(frame_id_t frame_id) : frame_id_(frame_id) { Reset(); }

void FrameHeader::EnsureData() {
  if (data_ == nullptr) {
    data_ = std::make_unique<data_t[]>(PAGE_SIZE);  // value-init → zero-filled
  }
}

auto FrameHeader::GetData() const -> const_data_ptr_t { return data_.get(); }

auto FrameHeader::GetDataMut() -> data_ptr_t { return data_.get(); }

void FrameHeader::Reset() {
  // A frame's latch protects whichever logical page is currently bound to the frame. Reconstructing
  // the unlocked latch on reuse gives the next page a fresh synchronization identity as well as a
  // fresh lock-order history. Without this, TSan joins unrelated page-lock orders across evictions
  // and reports ever-growing false deadlock cycles for a finite pool of recycled frame mutexes.
  std::destroy_at(&rwlatch_);
  std::construct_at(&rwlatch_);
  pin_count_.store(0);
  is_dirty_ = false;
  page_id_ = std::nullopt;
}

BufferPoolManager::BufferPoolManager(size_t num_frames, DiskManager *disk_manager, page_id_t initial_next_page_id)
    : num_frames_(num_frames),
      next_page_id_(initial_next_page_id),
      replacer_(std::make_unique<ArcReplacer>(num_frames)),
      disk_scheduler_(std::make_unique<DiskScheduler>(disk_manager)) {
  frames_.reserve(num_frames);
  page_table_.reserve(num_frames);
  for (size_t i = 0; i < num_frames; i++) {
    frames_.push_back(std::make_shared<FrameHeader>(static_cast<frame_id_t>(i)));
    free_frames_.push_back(static_cast<frame_id_t>(i));
  }
}

BufferPoolManager::~BufferPoolManager() = default;

auto BufferPoolManager::Size() const -> size_t { return num_frames_; }

auto BufferPoolManager::NewPage() -> page_id_t {
  {
    std::lock_guard lk(alloc_latch_);
    if (!free_pages_.empty()) {
      auto id = free_pages_.back();  // reuse a reclaimed id (positional layout refills its slot)
      free_pages_.pop_back();
      return id;
    }
  }
  return next_page_id_.fetch_add(1);
}

auto BufferPoolManager::GetFreePages() -> std::vector<page_id_t> {
  std::lock_guard lk(alloc_latch_);
  return free_pages_;
}

void BufferPoolManager::SetFreePages(std::vector<page_id_t> free_pages) {
  std::lock_guard lk(alloc_latch_);
  free_pages_ = std::move(free_pages);
}

auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  {
    std::lock_guard lk(latch_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
      auto frame_id = it->second;
      BUMBLEBEE_ASSERT(frame_id < static_cast<frame_id_t>(frames_.size()), "Frame out of index");
      auto frame = frames_[frame_id];
      if (frame->pin_count_ > 0) {
        return false;  // still pinned — not deleted, id not reclaimed
      }
      page_table_.erase(it);
      frame->Reset();
      free_frames_.push_back(frame_id);
      replacer_->Remove(frame_id);
    }
  }

  disk_scheduler_->DeallocatePage(page_id);
  {
    std::lock_guard lk(alloc_latch_);
    free_pages_.push_back(page_id);  // reclaim the id so NewPage can reuse its (now zero-filled) slot
  }
  return true;
}

auto BufferPoolManager::EvictOneLocked(std::unique_lock<std::mutex> &lk) -> bool {
  auto frame_evicted = replacer_->Evict();
  if (frame_evicted == std::nullopt) {
    return false;  // every frame is pinned
  }
  auto frame_id = frame_evicted.value();
  BUMBLEBEE_ASSERT(frame_id < static_cast<frame_id_t>(frames_.size()), "Frame out of index");
  auto frame = frames_[frame_id];
  BUMBLEBEE_ASSERT(frame->page_id_ != std::nullopt, "page id is null of evicted frame");
  auto page_id = frame->page_id_.value();

  if (!frame->is_dirty_) {
    // Clean victim: no I/O, so recycle it entirely under the lock (the fast, common case).
    page_table_.erase(page_id);
    frame->Reset();
    free_frames_.push_back(frame_id);
    return true;
  }

  // Dirty victim: flush OUTSIDE the pool latch. Detach it and mark the page as flushing first, so a
  // concurrent fetch of this same page blocks on page_io_cv_ instead of scheduling a read that would
  // race the in-flight write. The victim is no longer in the page table, the free list, or the
  // replacer, so no other thread can touch it except by waiting on page_io_cv_.
  page_table_.erase(page_id);
  flushing_.insert(page_id);
  lk.unlock();

  frame->rwlatch_.lock();  // exclusive; the victim is unpinned so no guard contends here
  DiskRequest request{true, frame->data_.get(), page_id, disk_scheduler_->CreatePromise()};
  auto future = request.callback_.get_future();
  disk_scheduler_->Schedule(request);
  bool ok = future.get();
  frame->rwlatch_.unlock();

  lk.lock();
  flushing_.erase(page_id);
  page_io_cv_.notify_all();
  if (!ok) {
    // The write failed: the page is not safely on disk, so re-map it (data intact) and re-register
    // it in the replacer. The caller sees no free frame (out of memory) and can retry.
    page_table_[page_id] = frame_id;
    replacer_->RecordAccess(frame_id, page_id);
    replacer_->SetEvictable(frame_id, true);
    return false;
  }
  frame->Reset();
  free_frames_.push_back(frame_id);
  return true;
}

auto BufferPoolManager::CheckedPage(page_id_t page_id, AccessType access_type) -> std::shared_ptr<FrameHeader> {
  while (true) {
    std::shared_ptr<FrameHeader> frame;
    bool needs_read = false;
    {
      std::unique_lock<std::mutex> lk(latch_);
      // A loading page is already mapped to its reserved frame, but its bytes are not readable yet.
      // An evicted page remains unavailable until its dirty write-back has completed.
      page_io_cv_.wait(
          lk, [&] { return loading_.find(page_id) == loading_.end() && flushing_.find(page_id) == flushing_.end(); });

      auto it = page_table_.find(page_id);
      if (it != page_table_.end()) {
        frame = frames_[it->second];
      } else {
        if (free_frames_.empty()) {
          if (!EvictOneLocked(lk)) {
            return nullptr;  // out of memory
          }
          continue;  // EvictOneLocked may have dropped the lock — restart and re-check the page table
        }

        auto frame_id = free_frames_.back();
        free_frames_.pop_back();
        frame = frames_[frame_id];
        frame->EnsureData();  // the one point a frame goes from unused to holding a page
        page_table_[page_id] = frame_id;
        frame->page_id_ = page_id;
        loading_.insert(page_id);
        needs_read = true;
      }
      frame->pin_count_.fetch_add(1);
      replacer_->RecordAccess(frame->frame_id_, page_id, access_type);
      replacer_->SetEvictable(frame->frame_id_, false);
    }

    if (needs_read) {
      // The frame is reserved and hidden behind `loading_`, so it is safe to latch only after
      // releasing the pool mutex. Keeping that lock order is important: callers commonly fetch a
      // second page while already holding a page guard, which establishes frame -> pool ordering.
      // Taking a frame latch under the pool mutex here would create the reverse edge and a potential
      // deadlock (reported by ThreadSanitizer as a lock-order inversion).
      frame->rwlatch_.lock();
      DiskRequest request{false, frame->data_.get(), page_id, disk_scheduler_->CreatePromise()};
      auto future = request.callback_.get_future();
      disk_scheduler_->Schedule(request);
      future.get();
      frame->rwlatch_.unlock();

      {
        std::lock_guard lk(latch_);
        loading_.erase(page_id);
      }
      page_io_cv_.notify_all();
    }
    return frame;
  }
}

auto BufferPoolManager::CheckedWritePage(page_id_t page_id, AccessType access_type) -> std::optional<WritePageGuard> {
  auto frame = CheckedPage(page_id, access_type);
  if (frame == nullptr) {
    return std::nullopt;
  }
  return WritePageGuard(page_id, frame, replacer_.get(), &latch_, disk_scheduler_.get());
}

auto BufferPoolManager::CheckedReadPage(page_id_t page_id, AccessType access_type) -> std::optional<ReadPageGuard> {
  auto frame = CheckedPage(page_id, access_type);
  if (frame == nullptr) {
    return std::nullopt;
  }
  return ReadPageGuard(page_id, frame, replacer_.get(), &latch_, disk_scheduler_.get());
}

auto BufferPoolManager::WritePage(page_id_t page_id, AccessType access_type) -> WritePageGuard {
  auto guard_opt = CheckedWritePage(page_id, access_type);
  if (!guard_opt.has_value()) {
    std::fprintf(stderr, "\n`CheckedWritePage` failed to bring in page %d\n", page_id);
    std::abort();
  }
  return std::move(guard_opt).value();
}

auto BufferPoolManager::ReadPage(page_id_t page_id, AccessType access_type) -> ReadPageGuard {
  auto guard_opt = CheckedReadPage(page_id, access_type);
  if (!guard_opt.has_value()) {
    std::fprintf(stderr, "\n`CheckedReadPage` failed to bring in page %d\n", page_id);
    std::abort();
  }
  return std::move(guard_opt).value();
}

auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  std::shared_ptr<FrameHeader> frame;
  {
    std::unique_lock<std::mutex> lk(latch_);
    page_io_cv_.wait(lk, [&] { return loading_.find(page_id) == loading_.end(); });
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
      return false;
    }
    auto frame_id = it->second;
    BUMBLEBEE_ASSERT(frame_id < static_cast<frame_id_t>(frames_.size()), "Frame out of index");
    frame = frames_[frame_id];

    frame->pin_count_.fetch_add(1);
    replacer_->SetEvictable(frame->frame_id_, false);
  }

  frame->rwlatch_.lock();
  if (frame->is_dirty_) {
    DiskRequest request{true, frame->data_.get(), frame->page_id_.value(), disk_scheduler_->CreatePromise()};
    auto future = request.callback_.get_future();
    disk_scheduler_->Schedule(request);
    future.get();
    frame->is_dirty_ = false;
  }
  frame->rwlatch_.unlock();

  auto pin = frame->pin_count_.fetch_sub(1);
  BUMBLEBEE_ASSERT(pin > 0, "Pin underflow");
  if (pin == 1) {
    std::lock_guard lk(latch_);
    if (frame->pin_count_ == 0) {
      replacer_->SetEvictable(frame->frame_id_, true);
    }
  }
  return true;
}

auto BufferPoolManager::GetAllPages() -> std::vector<page_id_t> {
  std::vector<page_id_t> pages;
  std::lock_guard lk(latch_);
  pages.reserve(page_table_.size());
  for (auto &[pid, _] : page_table_) {
    pages.push_back(pid);
  }
  return pages;
}

void BufferPoolManager::FlushAllPages() {
  for (auto &page_id : GetAllPages()) {
    FlushPage(page_id);
  }
}

auto BufferPoolManager::GetPinCount(page_id_t page_id) -> std::optional<size_t> {
  std::lock_guard lk(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return std::nullopt;
  }
  auto frame_id = it->second;
  BUMBLEBEE_ASSERT(frame_id < static_cast<frame_id_t>(frames_.size()), "Frame out of index");
  auto frame = frames_[frame_id];
  return frame->pin_count_.load();
}

}  // namespace bumblebee
