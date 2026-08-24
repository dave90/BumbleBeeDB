//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transaction_manager.h
//
// Identification: src/include/concurrency/transaction_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <chrono>  // NOLINT
#include <functional>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <shared_mutex>  // NOLINT
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "concurrency/transaction.h"
#include "concurrency/watermark.h"
#include "storage/table/rid.h"

namespace bumblebee {

class Catalog;

/**
 * @brief The per-page version side-table: the head UndoLink for each slot that has a version chain.
 *
 * Kept off the page so the page format stays version-agnostic. `mutex_` guards `prev_links_`; the
 * page latch remains the atomicity anchor for `(meta, row) + head-link` reads/writes.
 */
struct PageVersionInfo {
  std::shared_mutex mutex_;
  std::unordered_map<uint32_t, UndoLink> prev_links_;
};

/**
 * @brief Owns transaction lifecycle and MVCC metadata: id/timestamp allocation, the live-txn map,
 * the version side-table, and the GC watermark. There is deliberately NO lock manager.
 *
 * Snapshot Isolation is the default; Serializable adds commit-time backward validation.
 */
class TransactionManager {
 public:
  /** What one `GarbageCollection()` pass did: timeout aborts and reclaimed (destroyed) txn objects. */
  struct GcStats {
    /** RUNNING/TAINTED transactions older than the timeout that this pass aborted. */
    size_t timed_out_{0};
    /** Finished transactions removed from the txn map (their undo logs freed) by this pass. */
    size_t reclaimed_{0};
  };

  /**
   * @param catalog      Resolves table oids to heaps for commit-stamp / rollback (may be null in tests).
   * @param txn_timeout  How long a transaction may run before `GarbageCollection()` aborts it. The GC
   *                     pass is the timeout driver: whoever schedules GC every N minutes also enforces
   *                     this. Defaults to 2 hours.
   */
  explicit TransactionManager(Catalog *catalog = nullptr, duration_t txn_timeout = DEFAULT_TXN_TIMEOUT)
      : catalog_(catalog), txn_timeout_(txn_timeout) {}

  /** @brief Start a transaction; its read snapshot is the latest committed timestamp. */
  auto Begin(IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION) -> Transaction *;

  /** @brief Start a transaction and return a lifetime-safe owning handle. */
  auto BeginShared(IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION)
      -> std::shared_ptr<Transaction>;

  /** @brief Commit a transaction: assign a commit ts, publish its writes, advance the watermark. */
  auto Commit(Transaction *txn) -> bool;

  /** @brief Abort a transaction: roll its writes back to their pre-images. */
  void Abort(Transaction *txn);

  /**
   * @brief Abort every still-open (RUNNING/TAINTED) transaction, regardless of age.
   *
   * Used at database shutdown so no uncommitted, temp-stamped row — whose in-memory undo chain is
   * about to vanish — is flushed to disk; a reopened database must see only committed state. Assumes
   * no other thread is starting new transactions concurrently (the shutdown precondition).
   */
  void AbortAllRunning();

  /**
   * @brief Reclaim finished transactions whose versions no live snapshot can see, and enforce the
   * transaction timeout by aborting any transaction that has outlived `txn_timeout_`.
   *
   * This is the timeout driver: scheduling GC periodically (every N minutes) is what bounds a
   * transaction's lifetime — a timed-out txn is aborted here, then reclaimed in the same pass.
   *
   * @return What the pass did (timeout aborts, reclaimed txns) — the shell's `\gc` reports it.
   */
  auto GarbageCollection() -> GcStats;

  /**
   * @brief Look up a transaction's state by id, without exposing the object itself.
   *
   * GC *destroys* reclaimed transactions, so a caller holding a raw `Transaction *` across a GC pass
   * must not dereference it; this is the safe way to learn what became of a transaction. A collected
   * txn returns nullopt — since RUNNING/TAINTED txns are never collected, nullopt after GC means the
   * transaction finished (a timed-out one was aborted, then reclaimed).
   */
  auto GetTransactionState(txn_id_t txn_id) -> std::optional<TransactionState> {
    std::shared_lock lock(txn_map_mutex_);
    auto it = txn_map_.find(txn_id);
    if (it == txn_map_.end()) {
      return std::nullopt;
    }
    return it->second->GetTransactionState();
  }

  /**
   * @brief Serializable commit-time backward validation.
   *
   * @return True if `txn` may serialize (no concurrent commit invalidated one of its reads). Always
   *         true for a non-serializable or read-only txn.
   */
  auto VerifyTxn(Transaction *txn) -> bool;

  // --- Version side-table ---------------------------------------------------------------------

  /** @brief Get the head UndoLink for `rid`, or nullopt if it has no version chain. */
  auto GetUndoLink(RID rid) -> std::optional<UndoLink>;

  /**
   * @brief Set the head UndoLink for `rid`. If `check` is provided it runs under the version latch
   * on the current link; a false result aborts the update (returns false) — the write-write gate.
   */
  auto UpdateUndoLink(RID rid, std::optional<UndoLink> prev_link,
                      std::function<bool(std::optional<UndoLink>)> &&check = nullptr) -> bool;

  /** @brief Fetch an undo log by link, or nullopt if the owning txn was already collected. */
  auto GetUndoLogOptional(UndoLink link) -> std::optional<UndoLog>;

  /** @brief Fetch an undo log by link; throws if the owning txn is gone. */
  auto GetUndoLog(UndoLink link) -> UndoLog;

  auto GetWatermark() -> timestamp_t {
    std::unique_lock lock(txn_map_mutex_);
    return running_txns_.GetWatermark();
  }

  auto GetLastCommitTs() const -> timestamp_t { return last_commit_ts_.load(); }

  /**
   * @brief Seed the commit-timestamp high-water mark after recovery (clean-shutdown durability).
   *
   * Committed rows persist their commit ts in the on-page tuple header, but the counter that hands out
   * read/commit timestamps lives only in memory and would otherwise restart from 0 on reopen — making
   * every persisted row (ts >= 1) invisible to a fresh reader (whose read_ts would be 0), since the
   * in-memory undo chains that a reader falls back to are also gone. Restoring the high-water mark makes
   * new snapshots start at/after every persisted version. Call once at startup, before any Begin().
   */
  void SetLastCommitTs(timestamp_t ts) {
    std::unique_lock lock(txn_map_mutex_);
    last_commit_ts_.store(ts);
    running_txns_.UpdateCommitTs(ts);
  }

  /** @return The number of transactions still tracked (test observability for GC). */
  auto GetTransactionCount() -> size_t {
    std::shared_lock lock(txn_map_mutex_);
    return txn_map_.size();
  }

 private:
  Catalog *catalog_;
  /** A txn alive longer than this is aborted by `GarbageCollection()`. */
  const duration_t txn_timeout_;

  std::atomic<txn_id_t> next_txn_id_{TXN_START_ID};
  std::atomic<timestamp_t> last_commit_ts_{0};

  /** Serializes the commit-ts assignment + publish so commit order matches ts order. */
  std::mutex commit_mutex_;

  /** Guards `txn_map_` and `running_txns_` (the watermark). */
  std::shared_mutex txn_map_mutex_;
  std::unordered_map<txn_id_t, std::shared_ptr<Transaction>> txn_map_;
  Watermark running_txns_{0};

  /** Guards `version_info_` (the map of page-id → per-page version table). */
  std::shared_mutex version_info_mutex_;
  std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo>> version_info_;
};

}  // namespace bumblebee
