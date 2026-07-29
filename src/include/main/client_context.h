//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// client_context.h
//
// Identification: src/include/main/client_context.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <thread>

#include "catalog/catalog.h"
#include "common/config.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"
#include "main/query_memory_manager.h"

namespace bumblebee {

class BufferPoolManager;

/** @brief Per-session, per-query knobs. Read once, at scheduling time, on the client thread. */
struct ClientConfig {
  /** The upper bound on worker threads a query may use (clamped into `[1, MAX_THREADS]`). */
  idx_t max_threads_{DEFAULT_THREAD_COUNT};
  /** The per-query memory budget before an out-of-core operator spills. */
  idx_t max_memory_{MAX_MEMORY};
  /** When true, lowering picks the out-of-core join/sort variants regardless of size estimate. */
  bool prefer_external_{false};
  /** Heap pages per parallel-scan morsel; lowering it lets a small table span several morsels (tests). */
  idx_t morsel_pages_{MORSEL_PAGES};
  /** Local-table group count past which a hash-aggregate sink task switches to per-partition
   * sub-tables (zero-copy combine hand-off); lowering it lets tests exercise the partitioned
   * path with small tables. 0 keeps the default. */
  idx_t agg_partition_threshold_{0};
  /** Target rows per columnar (row-group) morsel. Reserved for the columnar scan path. */
  idx_t morsel_size_{MORSEL_SIZE};
};

/**
 * @brief One session's execution state — the query-scoped facilities every operator can reach.
 *
 * Lifetime is one session; a session runs its queries strictly sequentially, so at most one query
 * touches a `ClientContext` at a time. It carries the **active transaction** (one per statement in
 * autocommit), through which all storage access goes, plus the catalog, transaction manager and
 * (for spill, later) buffer pool.
 *
 * A `Database &` was considered here; because an in-memory instance has no `Database`, this holds the
 * individual facilities directly, which serves both the in-memory and durable backends identically.
 */
class ClientContext {
 public:
  ClientContext(Catalog &catalog, TransactionManager &txn_mgr, BufferPoolManager *bpm = nullptr)
      : catalog_(catalog), txn_mgr_(txn_mgr), bpm_(bpm) {
    const unsigned hw = std::thread::hardware_concurrency();
    const idx_t detected = hw == 0 ? DEFAULT_THREAD_COUNT : static_cast<idx_t>(hw);
    config_.max_threads_ = std::clamp<idx_t>(detected, 1, MAX_THREADS);
    mem_.SetBudget(config_.max_memory_);
  }

  Catalog &catalog_;
  TransactionManager &txn_mgr_;
  BufferPoolManager *bpm_;
  /** The statement's transaction; set by the driver (Begin) before execution, cleared after commit/abort. */
  Transaction *txn_{nullptr};
  ClientConfig config_;
  /** The per-query memory budget the out-of-core operators reserve against. */
  QueryMemoryManager mem_;
};

}  // namespace bumblebee
