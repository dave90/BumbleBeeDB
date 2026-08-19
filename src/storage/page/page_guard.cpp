//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// page_guard.cpp
//
// Identification: src/storage/page/page_guard.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/page/page_guard.h"

#include <memory>
#include <mutex>  // NOLINT
#include <utility>

#include "common/macros.h"
#include "storage/buffer/buffer_pool_manager.h"

namespace bumblebee {

ReadPageGuard::ReadPageGuard(page_id_t page_id, std::shared_ptr<FrameHeader> frame, ArcReplacer *replacer,
                             std::mutex *bpm_latch, DiskScheduler *disk_scheduler)
    : page_id_(page_id),
      frame_(std::move(frame)),
      replacer_(replacer),
      bpm_latch_(bpm_latch),
      disk_scheduler_(disk_scheduler) {
  is_valid_ = true;
  frame_->rwlatch_.lock_shared();
}

ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept {
  page_id_ = that.page_id_;
  frame_ = std::move(that.frame_);
  replacer_ = that.replacer_;
  bpm_latch_ = that.bpm_latch_;
  disk_scheduler_ = that.disk_scheduler_;
  is_valid_ = that.is_valid_;
  that.is_valid_ = false;
}

auto ReadPageGuard::operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard & {
  if (this == &that) {
    return *this;
  }
  Drop();
  page_id_ = that.page_id_;
  frame_ = std::move(that.frame_);
  replacer_ = that.replacer_;
  bpm_latch_ = that.bpm_latch_;
  disk_scheduler_ = that.disk_scheduler_;
  is_valid_ = that.is_valid_;
  that.is_valid_ = false;
  return *this;
}

auto ReadPageGuard::GetPageId() const -> page_id_t {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid read guard");
  return page_id_;
}

auto ReadPageGuard::GetData() const -> const_data_ptr_t {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid read guard");
  return frame_->GetData();
}

auto ReadPageGuard::IsDirty() const -> bool {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid read guard");
  return frame_->is_dirty_;
}

// Note: a `ReadPageGuard` has deliberately NO Flush(). Flushing under a shared latch would require
// upgrading to exclusive, which breaks the guard's immutability guarantee (another writer could slip
// in during the upgrade window). Flushing is a write-side / buffer-pool concern.

void ReadPageGuard::Drop() {
  if (!is_valid_) {
    return;
  }
  is_valid_ = false;
  frame_->rwlatch_.unlock_shared();
  std::lock_guard lk(*bpm_latch_);
  auto pin = frame_->pin_count_.fetch_sub(1);
  BUMBLEBEE_ASSERT(pin > 0, "Pin underflow");
  if (pin == 1) {
    replacer_->SetEvictable(frame_->frame_id_, true);
  }
}

ReadPageGuard::~ReadPageGuard() { Drop(); }

/**********************************************************************************************************************/

WritePageGuard::WritePageGuard(page_id_t page_id, std::shared_ptr<FrameHeader> frame, ArcReplacer *replacer,
                               std::mutex *bpm_latch, DiskScheduler *disk_scheduler)
    : page_id_(page_id),
      frame_(std::move(frame)),
      replacer_(replacer),
      bpm_latch_(bpm_latch),
      disk_scheduler_(disk_scheduler) {
  frame_->rwlatch_.lock();
  is_valid_ = true;
}

WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept {
  page_id_ = that.page_id_;
  frame_ = std::move(that.frame_);
  replacer_ = that.replacer_;
  bpm_latch_ = that.bpm_latch_;
  disk_scheduler_ = that.disk_scheduler_;
  is_valid_ = that.is_valid_;
  that.is_valid_ = false;
}

auto WritePageGuard::operator=(WritePageGuard &&that) noexcept -> WritePageGuard & {
  if (this == &that) {
    return *this;
  }
  Drop();
  page_id_ = that.page_id_;
  frame_ = std::move(that.frame_);
  replacer_ = that.replacer_;
  bpm_latch_ = that.bpm_latch_;
  disk_scheduler_ = that.disk_scheduler_;
  is_valid_ = that.is_valid_;
  that.is_valid_ = false;
  return *this;
}

auto WritePageGuard::GetPageId() const -> page_id_t {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid write guard");
  return page_id_;
}

auto WritePageGuard::GetData() const -> const_data_ptr_t {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid write guard");
  return frame_->GetData();
}

auto WritePageGuard::GetDataMut() -> data_ptr_t {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid write guard");
  frame_->is_dirty_ = true;
  return frame_->GetDataMut();
}

auto WritePageGuard::IsDirty() const -> bool {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid write guard");
  return frame_->is_dirty_;
}

void WritePageGuard::Flush() {
  BUMBLEBEE_ENSURE(is_valid_, "tried to use an invalid write guard");
  BUMBLEBEE_ASSERT(frame_->page_id_ != std::nullopt, "page id is null");
  if (!IsDirty()) {
    return;
  }
  DiskRequest request{true, frame_->data_.get(), frame_->page_id_.value(), disk_scheduler_->CreatePromise()};
  auto future = request.callback_.get_future();
  disk_scheduler_->Schedule(request);
  future.get();
  frame_->is_dirty_ = false;
}

void WritePageGuard::Drop() {
  if (!is_valid_) {
    return;
  }
  is_valid_ = false;
  frame_->rwlatch_.unlock();
  std::lock_guard lk(*bpm_latch_);
  auto pin = frame_->pin_count_.fetch_sub(1);
  BUMBLEBEE_ASSERT(pin > 0, "Pin underflow");
  if (pin == 1) {
    replacer_->SetEvictable(frame_->frame_id_, true);
  }
}

WritePageGuard::~WritePageGuard() { Drop(); }

}  // namespace bumblebee
