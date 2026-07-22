//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// spill_collection.h
//
// Identification: src/include/execution/spill/spill_collection.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "storage/table/table_heap.h"

namespace bumblebee {

/**
 * @brief A temporary, buffer-pool-backed row collection — the on-disk operators' spill unit.
 *
 * It is a private `TableHeap` used as scratch: append `DataChunk`s (packed to `RowLayout` bytes on
 * buffer-pool pages, which the pool evicts to disk under memory pressure), then read them back with a
 * plain scan. It is never versioned, never part of a table, and invisible to MVCC.
 *
 * Pages are reclaimed two ways: the destructor frees them (so every spill dies with its operator
 * state at query end), and `Free()` returns them EAGERLY the moment the collection is provably dead —
 * the grace join calls it when a partition pair is drained or split, so peak buffer-pool usage tracks
 * the live set, not the sum of every spill ever created.
 */
class SpillCollection {
 public:
  SpillCollection(BufferPoolManager *bpm, SchemaRef schema)
      : heap_(std::make_unique<TableHeap>(bpm, schema)), schema_(std::move(schema)) {}

  ~SpillCollection() { Free(); }
  SpillCollection(const SpillCollection &) = delete;
  auto operator=(const SpillCollection &) -> SpillCollection & = delete;

  /**
   * @brief Return every page to the buffer pool's free list. Idempotent; the collection is terminal
   * afterwards (no more appends or scans). The caller must have closed any scan over it first.
   */
  void Free() {
    if (!freed_) {
      freed_ = true;
      heap_->FreeAllPages();
    }
  }

  /**
   * @brief Append a batch of rows (copied onto spill pages).
   *
   * Safe to call from several tasks at once: `TableHeap::Append` synchronizes internally (the same
   * per-page latching the parallel write sinks rely on), and the row count is atomic. That is what
   * lets the grace join's partitioning sinks write the shared partitions in parallel.
   */
  void Append(DataChunk &chunk) {
    BUMBLEBEE_ASSERT(!freed_, "SpillCollection::Append after Free");
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap_->Append(chunk, rids);
    count_.fetch_add(chunk.GetSize(), std::memory_order_relaxed);
  }

  /** @brief A forward scan over everything appended so far. */
  auto MakeScan() -> std::unique_ptr<TableScan> {
    BUMBLEBEE_ASSERT(!freed_, "SpillCollection::MakeScan after Free");
    return heap_->MakeScan();
  }

  auto Count() const -> idx_t { return count_.load(std::memory_order_relaxed); }
  auto GetSchema() const -> const SchemaRef & { return schema_; }

 private:
  std::unique_ptr<TableHeap> heap_;
  SchemaRef schema_;
  std::atomic<idx_t> count_{0};
  /** Set by Free(). Not atomic: freeing is done by the exclusive owner (or the destructor). */
  bool freed_{false};
};

}  // namespace bumblebee
