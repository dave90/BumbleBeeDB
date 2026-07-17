//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_heap.h
//
// Identification: src/include/storage/table/table_heap.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <vector>

#include "catalog/schema.h"
#include "concurrency/transaction.h"  // ScanPredicate
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/page/page_guard.h"
#include "storage/row/row_layout.h"
#include "storage/table/rid.h"
#include "storage/table/table_storage.h"
#include "storage/table/tuple_meta.h"

namespace bumblebee {

class HeapScan;
class TransactionManager;
class Transaction;

/**
 * @brief A row-format table: a singly linked list of slotted pages in the buffer pool.
 *
 * Rows are stored in `RowLayout` format (validity prefix + fixed columns + inline varlen payload).
 * All access is vectorized: `MakeScan` gathers pages into `DataChunk`s and `Append` scatters a
 * `DataChunk` into pages, both through the SIMD `RowOperations` kernels.
 */
class TableHeap : public TableStorage {
  friend class HeapScan;

 public:
  TableHeap(BufferPoolManager *bpm, SchemaRef schema);

  /**
   * @brief Open an EXISTING heap over pages already on disk (recovery): adopt `first_page_id` /
   * `last_page_id` without allocating or re-initializing any page.
   */
  TableHeap(BufferPoolManager *bpm, SchemaRef schema, page_id_t first_page_id, page_id_t last_page_id);

  auto GetFormat() const -> StorageFormat override { return StorageFormat::ROW; }

  auto MakeScan(const std::vector<idx_t> &projection = {}) -> std::unique_ptr<TableScan> override;

  /**
   * @brief A visibility-filtered scan: each row is the version visible to `txn`'s snapshot.
   *
   * Rows with no visible version (deleted in the snapshot, or created after it) are dropped; older
   * versions are reconstructed from the undo chain. Falls back to a plain scan when `txn` is null.
   *
   * `predicate` filters the scan to the rows it matches (an empty predicate — the default — matches
   * every row, i.e. an unfiltered whole-table scan). For a SERIALIZABLE `txn` the same predicate is
   * the scan's recorded read set: because the scan returns exactly the rows it matches, recording it
   * makes commit-time phantom validation as precise as the filter (a whole-table default records the
   * conservative "true" predicate, flagging any concurrent write to the table).
   */
  auto MakeMvccScan(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, ScanPredicate predicate = {},
                    const std::vector<idx_t> &projection = {}) -> std::unique_ptr<TableScan>;

  void Append(DataChunk &chunk, Vector &out_rids) override;
  void Update(Vector &row_ids, DataChunk &chunk) override;
  void Delete(Vector &row_ids, idx_t count) override;
  void Fetch(Vector &row_ids, idx_t count, DataChunk &out) override;

  auto GetFirstPageId() const -> page_id_t { return first_page_id_; }
  auto GetLastPageId() const -> page_id_t { return last_page_id_; }
  auto GetLayout() const -> const RowLayout & { return layout_; }
  auto GetSchema() const -> const SchemaRef & { return schema_; }

  /**
   * @brief A chunk scattered into one contiguous buffer of RowLayout bytes, one row per entry.
   *
   * `buffer` owns the bytes; `RowAt(i)` points into it, `sizes[i]` is that row's byte length.
   */
  struct ScatteredRows {
    std::vector<data_t> buffer;
    std::vector<size_t> offsets;
    std::vector<uint32_t> sizes;
    auto RowAt(idx_t i) const -> const_data_ptr_t { return buffer.data() + offsets[i]; }
    auto Count() const -> idx_t { return offsets.size(); }
  };

  /** @brief Scatter every row of `chunk` into a contiguous RowLayout byte buffer (one SIMD pass). */
  auto ScatterChunk(DataChunk &chunk) -> ScatteredRows;

  /** @brief Insert one row's bytes into the heap, extending it if needed, returning its RID. */
  auto AppendRowBytes(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size) -> RID;

  /** @brief Acquire the read latch on the page holding `rid` (the MVCC atomicity anchor for reads). */
  auto AcquireTablePageReadLock(RID rid) -> ReadPageGuard { return bpm_->ReadPage(rid.GetPageId()); }
  /** @brief Acquire the write latch on the page holding `rid` (the MVCC atomicity anchor for writes). */
  auto AcquireTablePageWriteLock(RID rid) -> WritePageGuard { return bpm_->WritePage(rid.GetPageId()); }

 private:
  /** @brief Compute the byte sizes of every row in `chunk` (fixed width + varlen payloads). */
  auto ComputeRowSizes(DataChunk &chunk) const -> std::vector<uint32_t>;

  BufferPoolManager *bpm_;
  SchemaRef schema_;
  RowLayout layout_;

  std::mutex latch_;
  page_id_t first_page_id_;
  page_id_t last_page_id_;
};

}  // namespace bumblebee
