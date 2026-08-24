//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// index_maintenance.h
//
// Identification: src/include/execution/operator/persistent/index_maintenance.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

#include "catalog/catalog.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

class Transaction;
class TransactionManager;

// Index-aware DML for the bustub-style directory model: the primary-key index is a stable key -> RID map
// that DML never deletes from. Whether a key is "live" is read from the tuple's MVCC version at that RID,
// so INSERT (and a key-changing UPDATE's insert half) route through InsertOrRevive:
//  - a live version at the key  -> duplicate (throws);
//  - a tombstoned slot          -> revived in place (no new index entry);
//  - no entry at all            -> a fresh tuple is appended and (key -> RID) registered.
// DELETE only tombstones the tuple (no index change). Abort needs NO index undo: reverting the tuple (the
// existing heap rollback) restores each key's liveness. The B+ tree's atomic InsertEntry result
// arbitrates concurrent first publication: the loser aborts and its provisional heap row is
// tombstoned. Once a directory entry exists, normal MVCC write-conflict handling arbitrates
// concurrent attempts to revive that key.

/** @return The table's primary-key index (key columns == the PK), or nullptr if it has none. */
auto FindPrimaryKeyIndex(Catalog &catalog, const TableInfo &info) -> std::shared_ptr<IndexInfo>;

/**
 * @brief Insert one user-width chunk through the same auto-id, MVCC, and primary-key maintenance
 * path used by PhysicalInsert. The chunk excludes column 0 when `table.auto_id_` is true.
 * @return The number of rows inserted.
 */
auto InsertTableChunk(Catalog &catalog, TransactionManager *txn_mgr, Transaction *txn, TableInfo &table,
                      DataChunk &chunk) -> idx_t;

/**
 * @brief Insert `chunk`'s rows into `heap`, maintaining the unique primary-key index `pk`; RIDs of the
 * inserted/revived rows are written to `out_rids`.
 *
 * `chunk` must be flat and full-width (all table columns, `_id` already filled for an auto key).
 *
 * @param keys_are_fresh The caller guarantees no key in `chunk` has ever been in this index, so the
 *        lookup that classifies duplicate/revive/new cannot possibly hit. TRUE ONLY for an auto
 *        `_id`, whose values come from `TableInfo::next_id_` — a monotonic, persisted high-water
 *        mark that is never decremented and never reissued, so a deleted row's id is gone for good.
 *        It halves the B+tree work per row (one descent to insert, instead of a probe then an
 *        insert), which is ~6% of a single-row INSERT statement. A user-supplied key (a declared PK,
 *        or a key-changing UPDATE) must leave this false — there the lookup is the correctness check.
 */
void InsertOrRevive(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, IndexInfo &pk,
                    DataChunk &chunk, Vector &out_rids, bool keys_are_fresh = false);

/**
 * @brief For each row, whether its primary key differs between `old_chunk` and `new_chunk` (both flat and
 * aligned). Used by UPDATE to split rows into in-place updates (key unchanged) and delete+insert moves.
 */
auto PrimaryKeyChanged(IndexInfo &pk, DataChunk &old_chunk, DataChunk &new_chunk) -> std::vector<bool>;

}  // namespace bumblebee
