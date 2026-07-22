//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transaction_manager.cpp
//
// Identification: src/concurrency/transaction_manager.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "concurrency/transaction_manager.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "storage/mvcc/mvcc.h"
#include "storage/page/table_page.h"
#include "storage/table/table_heap.h"

namespace bumblebee {

namespace {

/** @brief Resolve a table's row-format heap from the catalog, or nullptr if it has no such backend. */
auto GetTableHeap(Catalog *catalog, table_oid_t oid) -> TableHeap * {
  if (catalog == nullptr) {
    return nullptr;
  }
  auto info = catalog->GetTable(oid);
  if (info == nullptr || info->storage_ == nullptr || info->storage_->GetFormat() != StorageFormat::ROW) {
    return nullptr;
  }
  return static_cast<TableHeap *>(info->storage_.get());
}

/**
 * @brief Does any version of `rid` relevant to `txn`'s read window match one of `txn`'s scan
 * predicates? If so, a concurrent commit changed a row `txn` read (or would read) — a conflict.
 *
 * The relevant window is every version newer than `txn`'s snapshot plus the one version it actually
 * read (the newest committed at/before its read ts). We walk the chain from the base, testing each
 * non-deleted version, and stop after the first version at/before the read ts (that boundary version
 * is the one the txn read, so it is tested too).
 *
 * @return True if a conflict was found.
 */
auto RidConflictsWithReads(TransactionManager *mgr, Transaction *txn, table_oid_t oid, TableHeap *heap, RID rid)
    -> bool {
  auto read_ts = txn->GetReadTs();
  const auto &layout = heap->GetLayout();

  // Snapshot base + head link under one page read latch.
  TupleMeta meta;
  std::vector<char> base_bytes;
  std::optional<UndoLink> head_link;
  {
    auto guard = heap->AcquireTablePageReadLock(rid);
    const auto *page = guard.As<TablePage>();
    auto [m, ptr, size] = page->GetRow(rid.GetSlotNum());
    meta = m;
    base_bytes.assign(ptr, ptr + size);
    head_link = mgr->GetUndoLink(rid);
  }

  auto matches = [&](bool is_deleted, const_data_ptr_t row) -> bool {
    return !is_deleted && txn->IsScanTuple(oid, layout, row);
  };

  // Test the base if it is a committed version (skip a still-uncommitted temp stamp).
  bool base_is_read_ts_version = false;
  if (!Transaction::IsTempTs(meta.ts_)) {
    if (matches(meta.is_deleted_, reinterpret_cast<const_data_ptr_t>(base_bytes.data()))) {
      return true;
    }
    base_is_read_ts_version = meta.ts_ <= read_ts;  // nothing newer than the base is committed here
  }
  if (base_is_read_ts_version) {
    return false;  // the base is the version txn read; no concurrent newer version exists
  }

  // Walk older versions, testing each, until (and including) the one visible at read_ts.
  auto link = head_link;
  while (link.has_value() && link->IsValid()) {
    auto undo = mgr->GetUndoLogOptional(*link);
    if (!undo.has_value()) {
      break;  // collected already — predates every live snapshot, so predates this read ts too
    }
    if (matches(undo->is_deleted_, reinterpret_cast<const_data_ptr_t>(undo->row_.data()))) {
      return true;
    }
    if (undo->ts_ <= read_ts) {
      break;  // this was the version txn read; older versions are irrelevant
    }
    link = undo->prev_version_;
  }
  return false;
}

}  // namespace

auto TransactionManager::Begin(IsolationLevel isolation_level) -> Transaction * {
  std::unique_lock lock(txn_map_mutex_);
  auto txn_id = next_txn_id_.fetch_add(1);
  auto read_ts = last_commit_ts_.load();
  auto txn = std::make_shared<Transaction>(txn_id, isolation_level);
  txn->read_ts_.store(read_ts);
  auto *txn_ref = txn.get();
  txn_map_.insert({txn_id, std::move(txn)});
  running_txns_.AddTxn(read_ts);
  return txn_ref;
}

auto TransactionManager::Commit(Transaction *txn) -> bool {
  // Serializable backward validation. A fast pre-check outside the commit lock fails obvious
  // conflicts early; the authoritative check runs under commit_mutex_, which serializes commits so
  // the committed-txn set cannot change under us.
  if (txn->GetIsolationLevel() == IsolationLevel::SERIALIZABLE && !VerifyTxn(txn)) {
    Abort(txn);
    return false;
  }

  std::unique_lock commit_lock(commit_mutex_);

  if (txn->state_.load() != TransactionState::RUNNING) {
    throw Exception("only a RUNNING transaction can commit");
  }

  if (txn->GetIsolationLevel() == IsolationLevel::SERIALIZABLE && !VerifyTxn(txn)) {
    commit_lock.unlock();
    Abort(txn);
    return false;
  }

  auto commit_ts = last_commit_ts_.load() + 1;

  // Stamp every tuple this txn wrote with the real commit timestamp (replacing its temp stamp), so a
  // snapshot taken at or after commit_ts sees the committed version. Done under commit_mutex_ and the
  // per-page write latch, before last_commit_ts_ is published. Empty write set → a no-op (read-only).
  for (const auto &[oid, rids] : txn->GetWriteSets()) {
    auto *heap = GetTableHeap(catalog_, oid);
    if (heap == nullptr) {
      continue;
    }
    for (const auto &rid : rids) {
      MvccStampCommit(*heap, rid, commit_ts);
    }
  }

  txn->commit_ts_.store(commit_ts);
  txn->state_.store(TransactionState::COMMITTED);
  last_commit_ts_.store(commit_ts);

  {
    std::unique_lock lock(txn_map_mutex_);
    running_txns_.UpdateCommitTs(commit_ts);
    running_txns_.RemoveTxn(txn->read_ts_.load());
  }
  return true;
}

void TransactionManager::Abort(Transaction *txn) {
  auto state = txn->state_.load();
  if (state != TransactionState::RUNNING && state != TransactionState::TAINTED) {
    throw Exception("only a RUNNING or TAINTED transaction can abort");
  }

  // Roll each write-set tuple back to its committed pre-image / tombstone fresh inserts. Snapshot the
  // write set under the txn latch first, so this is safe even when the caller is the GC reaper on a
  // different thread than the one that populated it.
  // The index is a stable key -> RID directory that DML never deletes from (bustub-style): whether a key
  // is live is read from the tuple's MVCC version, which the heap rollback above already restored. So an
  // aborted insert's fresh tuple is tombstoned and a revived slot returns to deleted — no index undo.
  for (const auto &[oid, rids] : txn->SnapshotWriteSets()) {
    auto *heap = GetTableHeap(catalog_, oid);
    if (heap == nullptr) {
      continue;
    }
    for (const auto &rid : rids) {
      MvccRollback(this, txn, *heap, rid);
    }
  }

  txn->state_.store(TransactionState::ABORTED);
  {
    std::unique_lock lock(txn_map_mutex_);
    running_txns_.RemoveTxn(txn->read_ts_.load());
  }
}

void TransactionManager::AbortAllRunning() {
  // Collect the live txns under a shared lock, release it, then abort each (Abort re-locks the map).
  std::vector<std::shared_ptr<Transaction>> live;
  {
    std::shared_lock lock(txn_map_mutex_);
    for (auto &[id, txn] : txn_map_) {
      auto state = txn->state_.load();
      if (state == TransactionState::RUNNING || state == TransactionState::TAINTED) {
        live.push_back(txn);
      }
    }
  }
  for (auto &txn : live) {
    auto state = txn->state_.load();
    if (state == TransactionState::RUNNING || state == TransactionState::TAINTED) {
      Abort(txn.get());
    }
  }
}

void TransactionManager::GarbageCollection() {
  // TODO(item 4): prune stale entries from version_info_. Reclaiming a committed txn below drops its
  // undo logs (the bulk of the memory), but the per-slot head UndoLinks it left in version_info_ are
  // not removed here. They are harmless — GetUndoLogOptional returns nullopt for a collected txn, so
  // a chain walk simply stops — but over a very long run the version_info_ maps grow unboundedly.
  // A full GC pass should, per page, drop head links whose owning txn is gone and whose slot's base
  // version is already visible to the watermark.
  //
  // TODO(item 5): run this automatically. Nothing calls GarbageCollection() on its own today — a
  // caller must trigger it. A policy (a background thread, or "every N commits" from Commit) should
  // drive it so old versions are reclaimed — and, per below, timed-out txns aborted — without an
  // explicit call.

  // Phase 0 — enforce the transaction timeout. Abort any RUNNING/TAINTED txn older than txn_timeout_.
  // Collect candidates under a shared lock, release it (Abort re-locks the map), then abort; each one
  // becomes ABORTED and is therefore reclaimed by the sweep below in this same pass.
  //
  // TODO(async-cancellation): this is best-effort for a txn that is ACTIVELY executing on another
  // thread at the instant it is reaped — the reaper rolls the write set back while the txn's own
  // thread may still be adding writes, so a write appended after SnapshotWriteSets() escapes rollback.
  // Safe for the intended target (idle/abandoned txns, no concurrent activity). A fully robust fix is
  // cooperative cancellation: an atomic `aborted_` flag on Transaction that the reaper sets, which the
  // write path (ApplyMvccModify etc.) and Commit check mid-operation and bail out on before mutating.
  std::vector<std::shared_ptr<Transaction>> expired;
  {
    std::shared_lock lock(txn_map_mutex_);
    auto now = steady_clock_t::now();
    for (auto &[id, txn] : txn_map_) {
      auto state = txn->state_.load();
      if ((state == TransactionState::RUNNING || state == TransactionState::TAINTED) &&
          txn->IsExpired(txn_timeout_, now)) {
        expired.push_back(txn);
      }
    }
  }
  for (auto &txn : expired) {
    auto state = txn->state_.load();
    if (state == TransactionState::RUNNING || state == TransactionState::TAINTED) {
      Abort(txn.get());
    }
  }

  timestamp_t watermark = GetWatermark();

  std::unique_lock lock(txn_map_mutex_);
  // A finished (committed/aborted) txn is collectible once no live snapshot can see any of its
  // versions — i.e. its commit ts is at or below the watermark. Aborted txns rolled their writes
  // back already, so they are collectible immediately.
  for (auto it = txn_map_.begin(); it != txn_map_.end();) {
    auto &txn = it->second;
    auto txn_state = txn->state_.load();
    bool collectible = false;
    if (txn_state == TransactionState::ABORTED) {
      collectible = true;
    } else if (txn_state == TransactionState::COMMITTED && txn->commit_ts_.load() <= watermark) {
      collectible = true;
    }
    if (collectible) {
      it = txn_map_.erase(it);
    } else {
      ++it;
    }
  }
}

auto TransactionManager::VerifyTxn(Transaction *txn) -> bool {
  if (txn->GetIsolationLevel() != IsolationLevel::SERIALIZABLE) {
    return true;
  }
  // A read-only txn is always serializable under MVCC — it observed one consistent snapshot.
  if (txn->GetWriteSets().empty()) {
    return true;
  }
  auto read_ts = txn->GetReadTs();

  // Snapshot the set of RIDs written by transactions that committed after this txn's snapshot. Their
  // writes are the only ones that could have invalidated a read of this txn.
  std::vector<std::pair<table_oid_t, RID>> suspects;
  {
    std::shared_lock lock(txn_map_mutex_);
    for (const auto &[id, other] : txn_map_) {
      if (other.get() == txn) {
        continue;
      }
      if (other->GetTransactionState() != TransactionState::COMMITTED) {
        continue;
      }
      if (other->GetCommitTs() <= read_ts) {
        continue;
      }
      for (const auto &[oid, rids] : other->GetWriteSets()) {
        for (const auto &rid : rids) {
          suspects.emplace_back(oid, rid);
        }
      }
    }
  }

  for (const auto &[oid, rid] : suspects) {
    auto *heap = GetTableHeap(catalog_, oid);
    if (heap == nullptr) {
      continue;
    }
    if (RidConflictsWithReads(this, txn, oid, heap, rid)) {
      return false;
    }
  }
  return true;
}

auto TransactionManager::GetUndoLink(RID rid) -> std::optional<UndoLink> {
  std::shared_ptr<PageVersionInfo> page_info;
  {
    std::shared_lock lock(version_info_mutex_);
    auto it = version_info_.find(rid.GetPageId());
    if (it == version_info_.end()) {
      return std::nullopt;
    }
    page_info = it->second;
  }
  std::shared_lock page_lock(page_info->mutex_);
  auto it = page_info->prev_links_.find(rid.GetSlotNum());
  if (it == page_info->prev_links_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto TransactionManager::UpdateUndoLink(RID rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check) -> bool {
  std::shared_ptr<PageVersionInfo> page_info;
  {
    std::unique_lock lock(version_info_mutex_);
    auto it = version_info_.find(rid.GetPageId());
    if (it == version_info_.end()) {
      page_info = std::make_shared<PageVersionInfo>();
      version_info_.emplace(rid.GetPageId(), page_info);
    } else {
      page_info = it->second;
    }
  }

  std::unique_lock page_lock(page_info->mutex_);
  if (check != nullptr) {
    std::optional<UndoLink> current;
    auto it = page_info->prev_links_.find(rid.GetSlotNum());
    if (it != page_info->prev_links_.end()) {
      current = it->second;
    }
    if (!check(current)) {
      return false;
    }
  }
  if (prev_link.has_value()) {
    page_info->prev_links_[rid.GetSlotNum()] = *prev_link;
  } else {
    page_info->prev_links_.erase(rid.GetSlotNum());
  }
  return true;
}

auto TransactionManager::GetUndoLogOptional(UndoLink link) -> std::optional<UndoLog> {
  std::shared_ptr<Transaction> txn;
  {
    std::shared_lock lock(txn_map_mutex_);
    auto it = txn_map_.find(link.prev_txn_);
    if (it == txn_map_.end()) {
      return std::nullopt;
    }
    txn = it->second;
  }
  return txn->GetUndoLog(link.prev_log_idx_);
}

auto TransactionManager::GetUndoLog(UndoLink link) -> UndoLog {
  auto log = GetUndoLogOptional(link);
  if (!log.has_value()) {
    throw Exception("undo log not found — owning transaction was already collected");
  }
  return *log;
}

}  // namespace bumblebee
