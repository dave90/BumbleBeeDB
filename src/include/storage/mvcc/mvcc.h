//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// mvcc.h
//
// Identification: src/include/storage/mvcc/mvcc.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <vector>

#include "common/config.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"
#include "storage/table/rid.h"
#include "storage/table/tuple_meta.h"

namespace bumblebee {

class TableHeap;
class DataChunk;
class Vector;

/**
 * @brief The MVCC write/read helpers — the "execution common" layer, restyled.
 *
 * These bridge the vectorized engine (DataChunk in/out) to the per-row version-chain machinery. The
 * page latch acquired inside each helper is the atomicity anchor: `(meta, row) + head undo-link` are
 * always read or written together under one latch, so the earlier-read timestamp used by the
 * write-write conflict test cannot go stale.
 */

/** @brief Insert every row of `chunk` as `txn`'s uncommitted version; write the RIDs into `out_rids`. */
void MvccInsert(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, DataChunk &chunk,
                Vector &out_rids);

/**
 * @brief The shared write path for a same-RID update (`is_delete=false`) or delete (`is_delete=true`),
 * running the 3-branch write-write conflict logic under one page write latch. Also used to *revive* a
 * tombstoned slot in place (write a new, undeleted version over a deleted base).
 */
void ApplyMvccModify(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, RID rid,
                     bool is_delete, const_data_ptr_t new_row, uint16_t new_size);

/**
 * @brief Update in place, same-RID, the `chunk.GetSize()` rows named by `rids` to `chunk`.
 *
 * Per row runs the 3-branch write-write conflict logic; a conflict taints `txn` and throws
 * `ExecutionException`. New rows must be the same byte size as the rows they replace.
 */
void MvccUpdate(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, Vector &rids,
                DataChunk &chunk);

/** @brief Tombstone the `count` rows named by `rids`, versioning the pre-image; conflicts throw. */
void MvccDelete(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, Vector &rids,
                idx_t count);

/**
 * @brief Reconstruct the version visible to `txn` from a base tuple already read under a page latch.
 *
 * Latch-free: the caller must have read `(meta, base_row, head_link)` together under one page latch;
 * the version chain is then walked through the transaction manager (which uses its own locks). This
 * is the shared core of both the point read and the vectorized scan — the scan calls it while
 * holding the page read guard it is already scanning, so it must not re-latch the page.
 *
 * @return The visible row's RowLayout bytes, or nullopt if no version is visible.
 */
auto ReconstructVisible(TransactionManager *txn_mgr, Transaction *txn, const TupleMeta &meta, const_data_ptr_t base_row,
                        uint16_t base_size, std::optional<UndoLink> head_link) -> std::optional<std::vector<char>>;

/**
 * @brief Reconstruct the version of `rid` visible to `txn`'s snapshot (acquires the page read latch).
 *
 * @return The visible row's RowLayout bytes, or nullopt if no version is visible (the row is deleted
 *         in the snapshot, or did not yet exist at the read timestamp).
 */
auto CollectVisibleVersion(TransactionManager *txn_mgr, Transaction *txn, TableHeap &heap, RID rid)
    -> std::optional<std::vector<char>>;

/**
 * @brief Visibility-aware fetch: decode the visible version of each of `count` `rids` into `out`.
 *
 * Rows with no visible version are skipped; `out`'s cardinality is the number of visible rows.
 * @return The number of visible rows written to `out`.
 */
auto MvccFetch(TransactionManager *txn_mgr, Transaction *txn, TableHeap &heap, Vector &rids, idx_t count,
               DataChunk &out) -> idx_t;

/** @brief Stamp the base tuple at `rid` with `commit_ts` (commit path; replaces the temp stamp). */
void MvccStampCommit(TableHeap &heap, RID rid, timestamp_t commit_ts);

/** @brief Roll the base tuple at `rid` back to its pre-image / tombstone a fresh insert (abort path). */
void MvccRollback(TransactionManager *txn_mgr, Transaction *txn, TableHeap &heap, RID rid);

}  // namespace bumblebee
