//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// arc_replacer.cpp
//
// Identification: src/storage/buffer/arc_replacer.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/buffer/arc_replacer.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>

namespace bumblebee {

ArcReplacer::ArcReplacer(size_t num_frames) : replacer_size_(num_frames) {}

auto ArcReplacer::EvictFromList(std::list<frame_id_t> &q, std::list<page_id_t> &ghost) -> std::optional<frame_id_t> {
  auto node = q.crbegin();
  while (node != q.crend()) {
    auto frame_id = *node;
    BUMBLEBEE_ASSERT(alive_map_.count(frame_id), "Frame ID not present in status map");
    if (alive_map_[frame_id]->evictable_) {
      auto status = alive_map_[frame_id];
      auto page_id = status->page_id_;
      q.erase(std::next(node).base());
      ghost.push_front(page_id);
      status->list_iter_ = ghost.begin();
      ghost_map_[page_id] = status;
      BUMBLEBEE_ASSERT(status->arc_status_ == ArcStatus::MFU || status->arc_status_ == ArcStatus::MRU,
                       "Frame Status is not correct.");
      if (status->arc_status_ == ArcStatus::MFU) {
        ghost_map_[page_id]->arc_status_ = ArcStatus::MFU_GHOST;
      } else {
        ghost_map_[page_id]->arc_status_ = ArcStatus::MRU_GHOST;
      }
      alive_map_.erase(frame_id);
      curr_size_--;
      return frame_id;
    }
    ++node;
  }
  return std::nullopt;
}

auto ArcReplacer::UnsafeEvict() -> std::optional<frame_id_t> {
  if (curr_size_ == 0) {
    return std::nullopt;
  }

  if (mru_.size() < mru_target_size_) {
    auto evicted = EvictFromList(mfu_, mfu_ghost_);
    if (evicted) {
      return evicted;
    }
    return EvictFromList(mru_, mru_ghost_);
  }
  auto evicted = EvictFromList(mru_, mru_ghost_);
  if (evicted) {
    return evicted;
  }
  return EvictFromList(mfu_, mfu_ghost_);
}

auto ArcReplacer::Evict() -> std::optional<frame_id_t> {
  std::lock_guard lock(latch_);
  return UnsafeEvict();
}

namespace {

auto UpdateMruTargetSize(ArcStatus arc_status, size_t mru_ghost_size, size_t mfu_ghost_size,
                         size_t mru_target_size) -> size_t {
  if (arc_status == ArcStatus::MRU_GHOST) {
    if (mru_ghost_size >= mfu_ghost_size) {
      mru_target_size += 1;
    } else {
      mru_target_size += (mru_ghost_size > 0) ? mfu_ghost_size / mru_ghost_size : 0;
    }
    return mru_target_size;
  }
  if (mfu_ghost_size >= mru_ghost_size) {
    mru_target_size = (mru_target_size > 0) ? mru_target_size - 1 : 0;
  } else {
    auto delta = (mfu_ghost_size > 0) ? mru_ghost_size / mfu_ghost_size : 0;
    mru_target_size = (mru_target_size >= delta) ? mru_target_size - delta : 0;
  }
  return mru_target_size;
}

}  // namespace

void ArcReplacer::AccessHits(frame_id_t frame_id, FrameStatus &status) {
  BUMBLEBEE_ASSERT(status.arc_status_ == ArcStatus::MFU || status.arc_status_ == ArcStatus::MRU,
                   "Frame Alive Status is not correct.");
  if (status.arc_status_ == ArcStatus::MRU) {
    // Promote from recently-used to frequently-used.
    mru_.erase(status.list_iter_);
    mfu_.push_front(frame_id);
    status.arc_status_ = ArcStatus::MFU;
    status.list_iter_ = mfu_.begin();
  } else {
    // Refresh position in the frequently-used list.
    mfu_.erase(status.list_iter_);
    mfu_.push_front(frame_id);
    status.list_iter_ = mfu_.begin();
  }
}

void ArcReplacer::AccessGhostHits(frame_id_t frame_id, page_id_t page_id, FrameStatus &status) {
  BUMBLEBEE_ASSERT(status.arc_status_ == ArcStatus::MFU_GHOST || status.arc_status_ == ArcStatus::MRU_GHOST,
                   "Frame Ghost Status is not correct.");
  mru_target_size_ = UpdateMruTargetSize(status.arc_status_, mru_ghost_.size(), mfu_ghost_.size(), mru_target_size_);
  mru_target_size_ = std::max<size_t>(std::min(mru_target_size_, replacer_size_), 0);

  if (status.arc_status_ == ArcStatus::MRU_GHOST) {
    mru_ghost_.erase(status.list_iter_);
  } else {
    mfu_ghost_.erase(status.list_iter_);
  }

  status.arc_status_ = ArcStatus::MFU;
  status.frame_id_ = frame_id;
  status.page_id_ = page_id;
  status.evictable_ = false;
  mfu_.push_front(frame_id);
  status.list_iter_ = mfu_.begin();
}

void ArcReplacer::RecordAccess(frame_id_t frame_id, page_id_t page_id, [[maybe_unused]] AccessType access_type) {
  std::lock_guard lock(latch_);
  if (alive_map_.count(frame_id) != 0U) {
    // Access hits an alive frame in mru_ or mfu_.
    auto status = alive_map_[frame_id];
    AccessHits(frame_id, *status);
  } else if (ghost_map_.count(page_id) != 0U) {
    // Access hits a ghost entry: resurrect it into mfu_.
    auto status = ghost_map_[page_id];
    AccessGhostHits(frame_id, page_id, *status);
    ghost_map_.erase(page_id);
    alive_map_[frame_id] = status;
  } else {
    // A brand-new page enters mru_.
    auto status = std::make_shared<FrameStatus>(page_id, frame_id, false, ArcStatus::MRU);
    mru_.push_front(frame_id);
    status->list_iter_ = mru_.begin();
    alive_map_[frame_id] = status;
  }

  // Keep the ghost lists bounded.
  if (mru_.size() + mru_ghost_.size() > replacer_size_ && !mru_ghost_.empty()) {
    ghost_map_.erase(mru_ghost_.back());
    mru_ghost_.pop_back();
  } else if (mru_.size() + mru_ghost_.size() + mfu_ghost_.size() + mfu_.size() > 2 * replacer_size_ &&
             !mfu_ghost_.empty()) {
    ghost_map_.erase(mfu_ghost_.back());
    mfu_ghost_.pop_back();
  }
}

void ArcReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::lock_guard lock(latch_);
  if (alive_map_.count(frame_id) == 0U) {
    throw std::range_error("Frame ID not found");
  }

  auto status = alive_map_[frame_id];
  if (set_evictable == status->evictable_) {
    return;
  }
  status->evictable_ = set_evictable;

  if (set_evictable) {
    ++curr_size_;
  } else {
    BUMBLEBEE_ASSERT(curr_size_ > 0, "curr_size_ underflow");
    --curr_size_;
  }
}

void ArcReplacer::Remove(frame_id_t frame_id) {
  std::lock_guard lock(latch_);
  if (alive_map_.count(frame_id) == 0U) {
    return;
  }
  auto status = alive_map_[frame_id];
  if (!status->evictable_) {
    throw std::range_error("Frame ID not evictable");
  }

  BUMBLEBEE_ASSERT(status->arc_status_ == ArcStatus::MFU || status->arc_status_ == ArcStatus::MRU,
                   "Frame Status is not correct.");
  if (status->arc_status_ == ArcStatus::MFU) {
    mfu_.erase(status->list_iter_);
  } else {
    mru_.erase(status->list_iter_);
  }
  alive_map_.erase(frame_id);
  --curr_size_;
}

auto ArcReplacer::Size() -> size_t {
  std::lock_guard lock(latch_);
  return curr_size_;
}

}  // namespace bumblebee
