//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// mvcc.cpp
//
// Identification: src/storage/mvcc/mvcc.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/mvcc/mvcc.h"

#include <optional>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "storage/page/table_page.h"
#include "storage/row/row_operations.h"
#include "storage/table/table_heap.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** @brief Taint the txn and raise a write-write conflict. */
[[noreturn]] static void ThrowWriteWriteConflict(Transaction *txn) {
  txn->SetTainted();
  throw ExecutionException("write-write conflict: a concurrent transaction modified this row");
}

/**
 * @brief The shared write path for update (is_delete=false) and delete (is_delete=true).
 *
 * Everything — reading `(meta, row)`, deciding the branch, appending the undo log, applying the base
 * change, installing the head undo-link — happens under one page write latch, so no writer to this
 * RID can interleave and the earlier-read timestamp cannot go stale.
 */
void ApplyMvccModify(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, RID rid,
                     bool is_delete, const_data_ptr_t new_row, uint16_t new_size) {
  auto guard = heap.AcquireTablePageWriteLock(rid);
  auto *page = guard.AsMut<TablePage>();
  auto slot = rid.GetSlotNum();
  auto [meta, old_ptr, old_size] = page->GetRow(slot);
  std::vector<char> old_bytes(old_ptr, old_ptr + old_size);

  auto read_ts = txn->GetReadTs();
  auto temp_ts = txn->GetTransactionTempTs();

  auto apply_base = [&]() {
    TupleMeta new_meta{temp_ts, is_delete};
    if (is_delete) {
      // A delete keeps the row bytes and only flips the tombstone flag.
      page->UpdateTupleMeta(new_meta, slot);
    } else if (!page->UpdateRow(new_meta, new_row, new_size, slot)) {
      // A size-changing update is compacted into the page; only a row too large for it is rejected.
      throw ExecutionException("updated row is too large to fit in its page");
    }
  };

  if (meta.ts_ == temp_ts) {
    // Branch 1: self-update of my own uncommitted version. Overwrite the base in place; the existing
    // undo log (if any) already holds the committed pre-image older snapshots need — do not add one.
    apply_base();
  } else if (meta.ts_ <= read_ts) {
    // Branch 2: the base is a committed version at/before my snapshot. Capture it as an undo log,
    // then overwrite the base with my temp-stamped version and publish the new head undo-link.
    UndoLog log;
    log.is_deleted_ = meta.is_deleted_;
    log.row_ = meta.is_deleted_ ? std::vector<char>{} : std::move(old_bytes);
    log.ts_ = meta.ts_;
    log.prev_version_ = txn_mgr->GetUndoLink(rid).value_or(UndoLink{});
    auto link = txn->AppendUndoLog(std::move(log));
    apply_base();
    txn_mgr->UpdateUndoLink(rid, link);
  } else {
    // Branch 3: another txn holds this row uncommitted (temp ts) or committed it after my snapshot.
    // First-committer-wins: abort.
    ThrowWriteWriteConflict(txn);
  }
  txn->AppendWriteSet(oid, rid);
}

/** @brief A flat Vector of `data_ptr_t` pointing at each row buffer, for the row→column decode. */
static auto MakePointerVector() -> Vector { return Vector{LogicalType{LogicalTypeId::UBIGINT}}; }

void MvccInsert(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, DataChunk &chunk,
                Vector &out_rids) {
  (void)txn_mgr;
  auto count = chunk.GetSize();
  if (count == 0) {
    return;
  }
  auto scattered = heap.ScatterChunk(chunk);
  auto temp_ts = txn->GetTransactionTempTs();
  auto rid_out = FlatVector::GetData<int64_t>(out_rids);
  for (idx_t i = 0; i < count; i++) {
    // A fresh insert has no prior version, so no undo log and no head undo-link.
    auto rid =
        heap.AppendRowBytes(TupleMeta{temp_ts, false}, scattered.RowAt(i), static_cast<uint16_t>(scattered.sizes[i]));
    txn->AppendWriteSet(oid, rid);
    rid_out[i] = rid.Get();
  }
}

void MvccUpdate(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, Vector &rids,
                DataChunk &chunk) {
  auto count = chunk.GetSize();
  if (count == 0) {
    return;
  }
  auto scattered = heap.ScatterChunk(chunk);
  auto rid_data = FlatVector::GetData<int64_t>(rids);
  for (idx_t i = 0; i < count; i++) {
    ApplyMvccModify(txn_mgr, txn, oid, heap, RID(rid_data[i]), /*is_delete=*/false, scattered.RowAt(i),
                    static_cast<uint16_t>(scattered.sizes[i]));
  }
}

void MvccDelete(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, Vector &rids,
                idx_t count) {
  auto rid_data = FlatVector::GetData<int64_t>(rids);
  for (idx_t i = 0; i < count; i++) {
    ApplyMvccModify(txn_mgr, txn, oid, heap, RID(rid_data[i]), /*is_delete=*/true, nullptr, 0);
  }
}

auto ReconstructVisible(TransactionManager *txn_mgr, Transaction *txn, const TupleMeta &meta, const_data_ptr_t base_row,
                        uint16_t base_size, std::optional<UndoLink> head_link) -> std::optional<std::vector<char>> {
  auto read_ts = txn->GetReadTs();
  auto temp_ts = txn->GetTransactionTempTs();

  // The base is visible if it is my own uncommitted write, or a version committed at/before my read.
  if (meta.ts_ == temp_ts || meta.ts_ <= read_ts) {
    if (meta.is_deleted_) {
      return std::nullopt;
    }
    return std::vector<char>(base_row, base_row + base_size);
  }

  // Otherwise reconstruct: walk older versions until one committed at/before my snapshot.
  auto link = head_link;
  while (link.has_value() && link->IsValid()) {
    auto undo = txn_mgr->GetUndoLogOptional(*link);
    if (!undo.has_value()) {
      break;  // the owning txn was already collected — that version predates every live snapshot.
    }
    if (undo->ts_ <= read_ts) {
      if (undo->is_deleted_) {
        return std::nullopt;
      }
      return undo->row_;
    }
    link = undo->prev_version_;
  }
  return std::nullopt;  // no version existed at the read timestamp.
}

auto CollectVisibleVersion(TransactionManager *txn_mgr, Transaction *txn, TableHeap &heap, RID rid)
    -> std::optional<std::vector<char>> {
  // Snapshot the base tuple and the head undo-link under one read latch (the atomicity anchor), then
  // reconstruct latch-free.
  auto guard = heap.AcquireTablePageReadLock(rid);
  const auto *page = guard.As<TablePage>();
  auto [meta, ptr, size] = page->GetRow(rid.GetSlotNum());
  auto head_link = txn_mgr->GetUndoLink(rid);
  return ReconstructVisible(txn_mgr, txn, meta, ptr, size, head_link);
}

auto MvccFetch(TransactionManager *txn_mgr, Transaction *txn, TableHeap &heap, Vector &rids, idx_t count,
               DataChunk &out) -> idx_t {
  auto rid_data = FlatVector::GetData<int64_t>(rids);

  // Own each visible row's bytes so `out` never references a released page frame.
  std::vector<std::vector<char>> visible;
  visible.reserve(count);
  for (idx_t i = 0; i < count; i++) {
    auto bytes = CollectVisibleVersion(txn_mgr, txn, heap, RID(rid_data[i]));
    if (bytes.has_value()) {
      visible.push_back(std::move(*bytes));
    }
  }

  auto vcount = static_cast<idx_t>(visible.size());
  Vector rows = MakePointerVector();
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < vcount; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(visible[i].data());
  }
  const auto &layout = heap.GetLayout();
  for (idx_t col_no = 0; col_no < layout.GetColumnCount(); col_no++) {
    RowOperations::FullScanColumn(layout, rows, out.data_[col_no], vcount, col_no, /*copy_strings=*/true);
  }
  out.SetCardinality(vcount);
  return vcount;
}

void MvccStampCommit(TableHeap &heap, RID rid, timestamp_t commit_ts) {
  auto guard = heap.AcquireTablePageWriteLock(rid);
  auto *page = guard.AsMut<TablePage>();
  auto meta = page->GetTupleMeta(rid.GetSlotNum());
  meta.ts_ = commit_ts;
  page->UpdateTupleMeta(meta, rid.GetSlotNum());
}

void MvccRollback(TransactionManager *txn_mgr, Transaction *txn, TableHeap &heap, RID rid) {
  auto guard = heap.AcquireTablePageWriteLock(rid);
  auto *page = guard.AsMut<TablePage>();
  auto slot = rid.GetSlotNum();
  auto head_link = txn_mgr->GetUndoLink(rid);

  if (head_link.has_value() && head_link->IsValid() && head_link->prev_txn_ == txn->GetTransactionId()) {
    // This txn versioned a committed pre-image: restore it and re-point the head at the older version.
    auto log = txn->GetUndoLog(head_link->prev_log_idx_);
    TupleMeta restored{log.ts_, log.is_deleted_};
    if (log.is_deleted_) {
      page->UpdateTupleMeta(restored, slot);
    } else if (!page->UpdateRow(restored, reinterpret_cast<const_data_ptr_t>(log.row_.data()),
                                static_cast<uint16_t>(log.row_.size()), slot)) {
      // The pre-image was smaller-or-equal (it fit before), so restoring it always fits.
      throw ExecutionException("failed to restore pre-image row on rollback");
    }
    if (log.prev_version_.IsValid()) {
      txn_mgr->UpdateUndoLink(rid, log.prev_version_);
    } else {
      txn_mgr->UpdateUndoLink(rid, std::nullopt);
    }
  } else {
    // A fresh insert by this txn: tombstone it so no snapshot ever sees it.
    auto meta = page->GetTupleMeta(slot);
    meta.ts_ = 0;
    meta.is_deleted_ = true;
    page->UpdateTupleMeta(meta, slot);
  }
}

}  // namespace bumblebee
