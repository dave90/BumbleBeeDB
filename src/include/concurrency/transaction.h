//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transaction.h
//
// Identification: src/include/concurrency/transaction.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <chrono>  // NOLINT
#include <cstdint>
#include <functional>
#include <mutex>  // NOLINT
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/exception.h"
#include "common/macros.h"
#include "storage/table/rid.h"

namespace bumblebee {

class RowLayout;

/** The lifecycle state of a transaction. */
enum class TransactionState { RUNNING = 0, TAINTED, COMMITTED = 100, ABORTED };

/** The isolation level a transaction runs at. */
enum class IsolationLevel { SNAPSHOT_ISOLATION, SERIALIZABLE };

/**
 * @brief A link to the previous version of a row: which txn holds it and where in that txn's log buffer.
 */
struct UndoLink {
  txn_id_t prev_txn_{INVALID_TXN_ID};
  int prev_log_idx_{0};

  auto operator==(const UndoLink &other) const -> bool {
    return prev_txn_ == other.prev_txn_ && prev_log_idx_ == other.prev_log_idx_;
  }
  /** @return True if this link points at a real previous version. */
  auto IsValid() const -> bool { return prev_txn_ != INVALID_TXN_ID; }
};

/**
 * @brief One historical version of a row, kept in the writing transaction's undo buffer.
 *
 * Stores the FULL pre-image row bytes (RowLayout format), not a per-column delta — our rows are
 * self-contained, so a version's bytes are directly usable when reconstructing what a snapshot sees.
 */
struct UndoLog {
  /** Whether the pre-image was a deleted row. */
  bool is_deleted_{false};
  /** The full pre-image row bytes (empty when is_deleted_). */
  std::vector<char> row_;
  /** The commit timestamp of this historical version. */
  timestamp_t ts_{0};
  /** Link to the still-older version. */
  UndoLink prev_version_{};
};

/** A predicate a serializable scan recorded: does a reconstructed row (RowLayout bytes) match it? */
using ScanPredicate = std::function<bool(const RowLayout &layout, const_data_ptr_t row)>;

/**
 * @brief A transaction: its snapshot timestamp, isolation level, undo buffer, and write/read sets.
 *
 * A running txn stamps rows it writes with its *temporary* timestamp (== its id, `>= TXN_START_ID`);
 * on commit those stamps are replaced by the real commit timestamp.
 */
class Transaction {
  friend class TransactionManager;

 public:
  explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION)
      : isolation_level_(isolation_level), txn_id_(txn_id), start_time_(steady_clock_t::now()) {}

  Transaction(const Transaction &) = delete;
  auto operator=(const Transaction &) -> Transaction & = delete;

  auto GetTransactionId() const -> txn_id_t { return txn_id_; }
  /** @return The temporary timestamp a running txn stamps its uncommitted writes with (== its id). */
  auto GetTransactionTempTs() const -> timestamp_t { return txn_id_; }

  /** @return Whether `ts` is a temporary (uncommitted) stamp equal to some txn's id, not a commit ts. */
  static auto IsTempTs(timestamp_t ts) -> bool { return ts >= TXN_START_ID; }
  auto GetIsolationLevel() const -> IsolationLevel { return isolation_level_; }
  auto GetTransactionState() const -> TransactionState { return state_.load(); }
  auto GetReadTs() const -> timestamp_t { return read_ts_.load(); }
  auto GetCommitTs() const -> timestamp_t { return commit_ts_.load(); }

  /**
   * @brief Mark the transaction cooperatively cancelled if it is still open.
   * @return True only for the caller that first requested cancellation.
   */
  auto RequestCancellation() -> bool {
    std::lock_guard lock(finalize_mutex_);
    const auto state = state_.load(std::memory_order_acquire);
    if (state != TransactionState::RUNNING && state != TransactionState::TAINTED) {
      return false;
    }
    bool expected = false;
    return cancellation_requested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }

  auto IsCancellationRequested() const -> bool {
    return cancellation_requested_.load(std::memory_order_acquire);
  }

  /** @brief Enter/leave an executing statement so timeout GC never rolls back underneath it. */
  void EnterStatement() {
    if (IsCancellationRequested()) {
      throw ExecutionException("transaction was cancelled by its timeout");
    }
    active_statements_.fetch_add(1, std::memory_order_acq_rel);
    const auto state = state_.load(std::memory_order_acquire);
    if (IsCancellationRequested() || (state != TransactionState::RUNNING && state != TransactionState::TAINTED)) {
      active_statements_.fetch_sub(1, std::memory_order_acq_rel);
      throw ExecutionException("transaction is no longer active");
    }
  }

  void LeaveStatement() { active_statements_.fetch_sub(1, std::memory_order_acq_rel); }
  auto ActiveStatementCount() const -> size_t { return active_statements_.load(std::memory_order_acquire); }

  void ThrowIfCancellationRequested() const {
    if (IsCancellationRequested()) {
      throw ExecutionException("transaction was cancelled by its timeout");
    }
  }

  /** @return True if the txn has been alive at least `timeout` as of `now` (a timeout candidate). */
  auto IsExpired(duration_t timeout, time_point_t now) const -> bool { return now - start_time_ >= timeout; }

  /** @brief Overwrite one of this txn's undo logs in place. */
  void ModifyUndoLog(int log_idx, UndoLog new_log) {
    std::scoped_lock lock(latch_);
    undo_logs_[log_idx] = std::move(new_log);
  }

  /** @brief Append an undo log; return a link to it. Logs are never removed (links reference them). */
  auto AppendUndoLog(UndoLog log) -> UndoLink {
    std::scoped_lock lock(latch_);
    undo_logs_.push_back(std::move(log));
    return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
  }

  /** @brief Get a copy of one of this txn's undo logs. */
  auto GetUndoLog(size_t log_id) -> UndoLog {
    std::scoped_lock lock(latch_);
    return undo_logs_[log_id];
  }

  /** @brief Record that this txn wrote `rid` in table `oid`. */
  void AppendWriteSet(table_oid_t oid, RID rid) {
    std::scoped_lock lock(latch_);
    write_set_[oid].insert(rid);
  }

  auto GetWriteSets() -> const std::unordered_map<table_oid_t, std::unordered_set<RID>> & { return write_set_; }

  /** @brief A latch-safe copy of the write set — safe to read from another thread (e.g. the GC reaper). */
  auto SnapshotWriteSets() -> std::unordered_map<table_oid_t, std::unordered_set<RID>> {
    std::scoped_lock lock(latch_);
    return write_set_;
  }

  /** @brief Serializable: record a predicate for everything this txn read in table `oid`. */
  void AppendScanPredicate(table_oid_t oid, ScanPredicate pred) {
    std::scoped_lock lock(latch_);
    scan_predicates_[oid].push_back(std::move(pred));
  }

  /** @return True if any recorded predicate for `oid` matches `row`. */
  auto IsScanTuple(table_oid_t oid, const RowLayout &layout, const_data_ptr_t row) -> bool {
    std::scoped_lock lock(latch_);
    auto it = scan_predicates_.find(oid);
    if (it == scan_predicates_.end()) {
      return false;
    }
    for (auto &pred : it->second) {
      if (pred(layout, row)) {
        return true;
      }
    }
    return false;
  }

  /** @brief Mark this txn as needing abort (RUNNING → TAINTED). */
  void SetTainted() {
    auto state = state_.load();
    if (state != TransactionState::RUNNING) {
      throw Exception("only a RUNNING transaction can be tainted");
    }
    state_.store(TransactionState::TAINTED);
  }

 private:
  std::atomic<TransactionState> state_{TransactionState::RUNNING};
  std::atomic<timestamp_t> read_ts_{0};
  std::atomic<timestamp_t> commit_ts_{INVALID_TXN_ID};
  std::atomic<bool> cancellation_requested_{false};
  std::atomic<size_t> active_statements_{0};

  /** Serializes cancellation admission with commit/rollback finalization. */
  std::mutex finalize_mutex_;

  std::mutex latch_;  // protects undo_logs_, write_set_, scan_predicates_
  std::vector<UndoLog> undo_logs_;
  std::unordered_map<table_oid_t, std::unordered_set<RID>> write_set_;
  std::unordered_map<table_oid_t, std::vector<ScanPredicate>> scan_predicates_;

  const IsolationLevel isolation_level_;
  const txn_id_t txn_id_;
  /** When this txn began (steady clock); a txn older than the manager's timeout is aborted by GC. */
  const time_point_t start_time_;
};

}  // namespace bumblebee
