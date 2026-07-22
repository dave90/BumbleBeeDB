//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// top_n_heap.h
//
// Identification: src/include/execution/sort/top_n_heap.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <vector>

#include "common/config.h"
#include "common/helper.h"
#include "type/string_heap.h"
#include "type/vector/data_chunk.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

/**
 * The n-bounded heap behind PhysicalTopN, fed and drained one DataChunk at a time.
 *
 * The heap itself holds only lightweight entries — the row's encoded sort key (a string_t
 * over `key_heap_`) plus its row index into `payload_` — while the row data stays columnar:
 * qualifying rows are appended to `payload_` in one selection-vector Append per chunk, and
 * GetData emits the survivors by slicing `payload_`, so no Value is ever materialized.
 * Evicted rows are not erased eagerly; Reduce() compacts `payload_` (and the key bytes)
 * down to the live entries once enough dead rows accumulate, keeping memory O(limit).
 *
 * When the first sort key is an integer, a prefilter maps it to a monotonic order-code:
 * once the heap is full, rows (often whole chunks) whose first-column code cannot beat the
 * current worst entry are dropped before their sort keys are even built.
 */
class TopNHeap {
 public:
  /**
   * @param payload_types The column types of the rows offered to Sink.
   * @param key_types The types of the evaluated ORDER BY columns, in key order.
   * @param modifiers One per key column: ASC or DESC.
   * @param limit The n of top-n: the maximum number of rows kept.
   */
  TopNHeap(const std::vector<LogicalType> &payload_types, const std::vector<LogicalType> &key_types,
           const std::vector<OrderModifiers> &modifiers, idx_t limit);

  /**
   * @brief Offer every row of `input` to the heap.
   *
   * @param input The candidate rows.
   * @param keys The evaluated ORDER BY columns of those rows, row-aligned with `input`.
   */
  void Sink(DataChunk &input, DataChunk &keys);

  /** @brief Merge `other` (a worker's local heap) into this one. Finalizes `other`. */
  void Combine(TopNHeap &other);

  /** @brief Sort the entries into output order and compact the payload to match. */
  void Finalize();

  /**
   * @brief Emit up to STANDARD_VECTOR_SIZE rows starting at `pos`, in sorted order.
   *
   * Zero-copy: `output` becomes a dictionary over the compacted payload, so it is only
   * valid while this heap lives. Requires Finalize() to have run.
   *
   * @return The number of rows emitted; 0 when `pos` is past the end.
   */
  auto GetData(DataChunk &output, idx_t pos) -> idx_t;

  /** @return The number of rows currently held. */
  auto GetSize() const -> idx_t { return heap_.size(); }

 private:
  /** One kept row: its encoded sort key and its row index into `payload_`. */
  struct TopNEntry {
    string_t key_;
    idx_t index_;
    /** Order-code of the first sort column (see OrderCodeAt), cached for the prefilter. */
    uint64_t first_code_{0};
    bool first_valid_{false};

    /** Max-heap on the key: the front entry is the current worst row (the first evicted). */
    auto operator<(const TopNEntry &other) const -> bool { return key_ < other.key_; }
  };

  /** @return True if a row with this sort key belongs in the heap. */
  auto ShouldAdd(const string_t &key) const -> bool {
    return heap_.size() < limit_ || key < heap_.front().key_;
  }

  /** @brief Push `entry`, evicting the current worst when the heap is full. */
  void Push(const TopNEntry &entry) {
    if (heap_.size() >= limit_) {
      std::pop_heap(heap_.begin(), heap_.end());
      heap_.pop_back();
    }
    heap_.push_back(entry);
    std::push_heap(heap_.begin(), heap_.end());
  }

  /**
   * @brief Build the sort keys of `count` rows of `keys` and push the qualifying ones.
   *
   * @param sel Maps a `keys` row back to its `input` row; null when they are aligned.
   */
  void SinkRows(DataChunk &input, DataChunk &keys, const SelectionVector *sel, idx_t count);

  /**
   * @brief The value of row `i` of `v` (flat, integer) as a uint64 preserving sort order:
   * a smaller code sorts earlier under the first key's ASC/DESC.
   */
  auto OrderCodeAt(Vector &v, idx_t i) const -> uint64_t;

  /** @brief Compact the payload and the key bytes down to the live entries. */
  void Reduce(bool force);

  std::vector<TopNEntry> heap_;
  std::vector<OrderModifiers> modifiers_;
  idx_t limit_;

  /** The columns of every appended row; compacted to the live entries by Reduce(). */
  DataChunk payload_;
  /** Owns the bytes of every entry's sort key; rebuilt by Reduce(). */
  StringHeap key_heap_;

  // First-sort-column prefilter configuration (set in the constructor).
  bool prefilter_enabled_{false};
  bool first_desc_{false};
  PhysicalType first_type_{PhysicalType::STRING};

  // Cached selections, sized once in the constructor.
  SelectionVector append_sel_;
  SelectionVector cand_sel_;

  bool finalized_{false};
};

}  // namespace bumblebee
