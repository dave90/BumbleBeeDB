//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// watermark.cpp
//
// Identification: src/concurrency/watermark.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "concurrency/watermark.h"

#include "common/exception.h"

namespace bumblebee {

void Watermark::AddTxn(timestamp_t read_ts) {
  if (read_ts < commit_ts_) {
    throw Exception("read ts < commit ts in watermark");
  }
  bool was_empty = current_reads_.empty();
  ++current_reads_[read_ts];
  // The watermark is the smallest live read ts. It only lowers when the set was empty (this becomes
  // the sole key) or when a smaller ts than any seen is added.
  if (was_empty || read_ts < watermark_) {
    watermark_ = read_ts;
  }
}

void Watermark::RemoveTxn(timestamp_t read_ts) {
  auto it = current_reads_.find(read_ts);
  if (it == current_reads_.end()) {
    throw Exception("removing a read ts not present in watermark");
  }
  if (--it->second == 0) {
    current_reads_.erase(it);
  }
  // Recompute the watermark from the smallest remaining key. Guard the empty-set deref (the bug the
  // audit flagged): with no live reads the watermark is meaningless and GetWatermark falls back to
  // commit_ts_, so we simply leave watermark_ untouched rather than dereferencing begin().
  if (!current_reads_.empty()) {
    watermark_ = current_reads_.begin()->first;
  }
}

}  // namespace bumblebee
