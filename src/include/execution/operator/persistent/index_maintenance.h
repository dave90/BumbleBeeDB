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
// existing heap rollback) restores each key's liveness. What is still NOT handled is cross-transaction
// isolation (two concurrent txns can both see a key as free and both revive/insert it).

/** @return The table's primary-key index (key columns == the PK), or nullptr if it has none. */
auto FindPrimaryKeyIndex(Catalog &catalog, const TableInfo &info) -> std::shared_ptr<IndexInfo>;

/**
 * @brief Insert `chunk`'s rows into `heap`, maintaining the unique primary-key index `pk`; RIDs of the
 * inserted/revived rows are written to `out_rids`.
 *
 * `chunk` must be flat and full-width (all table columns, `_id` already filled for an auto key).
 */
void InsertOrRevive(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, IndexInfo &pk,
                    DataChunk &chunk, Vector &out_rids);

/**
 * @brief For each row, whether its primary key differs between `old_chunk` and `new_chunk` (both flat and
 * aligned). Used by UPDATE to split rows into in-place updates (key unchanged) and delete+insert moves.
 */
auto PrimaryKeyChanged(IndexInfo &pk, DataChunk &old_chunk, DataChunk &new_chunk) -> std::vector<bool>;

}  // namespace bumblebee
