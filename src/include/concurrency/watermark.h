//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// watermark.h
//
// Identification: src/include/concurrency/watermark.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <map>

#include "common/config.h"

namespace bumblebee {

/**
 * @brief Tracks the oldest read timestamp of any live transaction — the GC watermark.
 *
 * A version older than the watermark is invisible to every live snapshot, so it (and any undo log
 * for it) can be reclaimed. Read timestamps are held in a multiset-by-count map so repeated
 * timestamps are reference-counted; the watermark is the smallest live key.
 */
class Watermark {
 public:
  explicit Watermark(timestamp_t commit_ts) : commit_ts_(commit_ts), watermark_(commit_ts) {}

  /** @brief Register a live transaction reading at `read_ts`. */
  void AddTxn(timestamp_t read_ts);

  /** @brief Deregister a transaction that was reading at `read_ts`. */
  void RemoveTxn(timestamp_t read_ts);

  /** @brief Advance the notion of the latest committed timestamp. */
  void UpdateCommitTs(timestamp_t commit_ts) { commit_ts_ = commit_ts; }

  /** @return The oldest read timestamp of any live txn, or the latest commit ts if none are live. */
  auto GetWatermark() const -> timestamp_t {
    if (current_reads_.empty()) {
      return commit_ts_;
    }
    return watermark_;
  }

 private:
  timestamp_t commit_ts_;
  timestamp_t watermark_;
  /** read_ts -> number of live txns reading at it (a reference-counted ordered multiset). */
  std::map<timestamp_t, int> current_reads_;
};

}  // namespace bumblebee
