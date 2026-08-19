//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// arc_replacer.h
//
// Identification: src/include/storage/buffer/arc_replacer.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <list>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <unordered_map>

#include "common/config.h"
#include "common/macros.h"

namespace bumblebee {

/** The kind of access recorded against a frame. Only used to distinguish access patterns. */
enum class AccessType { Unknown = 0, Lookup, Scan, Index };

/** Which of the four ARC lists a frame currently belongs to. */
enum class ArcStatus { MRU, MFU, MRU_GHOST, MFU_GHOST };

/** Per-frame bookkeeping: where it lives and how to reach it in its list. */
struct FrameStatus {
  page_id_t page_id_;
  frame_id_t frame_id_;
  bool evictable_;
  ArcStatus arc_status_;
  std::list<frame_id_t>::iterator list_iter_;
  FrameStatus(page_id_t pid, frame_id_t fid, bool ev, ArcStatus st)
      : page_id_(pid), frame_id_(fid), evictable_(ev), arc_status_(st) {}
};

/**
 * @brief The ARC (Adaptive Replacement Cache) page-replacement policy.
 *
 * Four lists partition history: `mru_` (T1, recently seen once), `mfu_` (T2, seen more than once),
 * and their two ghost lists `mru_ghost_` (B1) / `mfu_ghost_` (B2), which remember recently evicted
 * pages so the target split `mru_target_size_` (the paper's `p`) can adapt to the workload.
 */
class ArcReplacer {
 public:
  explicit ArcReplacer(size_t num_frames);

  ArcReplacer(const ArcReplacer &) = delete;
  auto operator=(const ArcReplacer &) -> ArcReplacer & = delete;
  ArcReplacer(ArcReplacer &&) = delete;
  auto operator=(ArcReplacer &&) -> ArcReplacer & = delete;

  ~ArcReplacer() = default;

  /** @brief Evict a frame per the ARC policy, moving its page to the matching ghost list. */
  auto Evict() -> std::optional<frame_id_t>;

  /** @brief Record an access to `frame_id` holding `page_id`, updating the ARC lists. */
  void RecordAccess(frame_id_t frame_id, page_id_t page_id, AccessType access_type = AccessType::Unknown);

  /** @brief Toggle whether a frame may be evicted; adjusts the evictable count. */
  void SetEvictable(frame_id_t frame_id, bool set_evictable);

  /** @brief Remove an evictable frame from the replacer entirely. */
  void Remove(frame_id_t frame_id);

  /** @return The number of evictable frames. */
  auto Size() -> size_t;

 private:
  auto EvictFromList(std::list<frame_id_t> &q, std::list<page_id_t> &ghost) -> std::optional<frame_id_t>;
  auto UnsafeEvict() -> std::optional<frame_id_t>;
  void AccessHits(frame_id_t frame_id, FrameStatus &status);
  void AccessGhostHits(frame_id_t frame_id, page_id_t page_id, FrameStatus &status);

  std::list<frame_id_t> mru_;
  std::list<frame_id_t> mfu_;
  std::list<page_id_t> mru_ghost_;
  std::list<page_id_t> mfu_ghost_;

  /** Alive frames (in mru_ / mfu_), keyed by frame id. */
  std::unordered_map<frame_id_t, std::shared_ptr<FrameStatus>> alive_map_;
  /** Ghost entries (in mru_ghost_ / mfu_ghost_), keyed by page id. */
  std::unordered_map<page_id_t, std::shared_ptr<FrameStatus>> ghost_map_;

  /** The number of alive, evictable frames. */
  size_t curr_size_{0};
  /** The adaptive target size of mru_ (the paper's `p`). */
  size_t mru_target_size_{0};
  /** The replacer capacity (the paper's `c`). */
  size_t replacer_size_;
  std::mutex latch_;
};

}  // namespace bumblebee
