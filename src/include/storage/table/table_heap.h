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

#include <atomic>
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
class TableHeap;
class TransactionManager;
class Transaction;

/**
 * @brief The shared, snapshotted state a parallel scan hands morsels out from.
 *
 * A `TableHeap` is a singly linked page chain, so page *k* is not addressable without walking from the
 * head — two tasks cannot carve up the chain directly. `BeginParallelScan` fixes this by snapshotting
 * the heap's page directory (a `vector<page_id_t>`) plus the tail page's live-slot count (the Halloween
 * boundary) into this object. Workers then pull page-index morsels off `next_page_idx_` with a single
 * `fetch_add` — no per-morsel lock (that is the divergence from BumbleBee).
 *
 * The scan configuration (txn, predicate, projection) lives here too, recorded once at scan open, so
 * every morsel's `HeapScan` reconstructs the same visible version and the SERIALIZABLE read set is
 * recorded exactly once rather than per morsel.
 */
struct ParallelScanState {
  TableHeap *heap_{nullptr};
  /** The page directory as it stood when the scan opened. Pages linked later are not part of the scan. */
  std::vector<page_id_t> pages_;
  /** The live-slot count of the last snapshotted page at open — the Halloween bound on the tail page. */
  uint32_t stop_slot_{0};
  /** Column indices to materialize; empty means every column. */
  std::vector<idx_t> projection_;
  TransactionManager *txn_mgr_{nullptr};
  Transaction *txn_{nullptr};
  /** Row filter; empty ⇒ match every row. Also the SERIALIZABLE read set (recorded once at open). */
  ScanPredicate predicate_;
  /** The monotonic morsel cursor — a `fetch_add` needs no mutex over a monotonic page-index range. */
  std::atomic<idx_t> next_page_idx_{0};
  /** Heap pages handed out per morsel (>= 1). Seeded from `ClientConfig::morsel_pages_` at scan open. */
  idx_t morsel_pages_{MORSEL_PAGES};

  /** @brief Claim the next `[begin, end)` page-index morsel. @return false when the cursor is exhausted. */
  auto NextMorsel(idx_t &begin, idx_t &end) -> bool;

  auto NumPages() const -> idx_t { return pages_.size(); }
};

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

  /**
   * @brief Open a parallel scan: snapshot the page directory + Halloween boundary + read set once.
   *
   * The returned state is shared by a `PhysicalTableScan`'s global source state; each task opens a
   * per-morsel cursor via `MakeMorselScan`. `txn`/`predicate` may be null/empty for a plain scan.
   */
  auto BeginParallelScan(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, ScanPredicate predicate = {},
                         const std::vector<idx_t> &projection = {}) -> std::shared_ptr<ParallelScanState>;

  /** @brief Open a `TableScan` cursor over the `[begin, end)` page-index morsel of a parallel scan. */
  auto MakeMorselScan(const std::shared_ptr<ParallelScanState> &state, idx_t begin, idx_t end)
      -> std::unique_ptr<TableScan>;

  void Append(DataChunk &chunk, Vector &out_rids) override;
  void Update(Vector &row_ids, DataChunk &chunk) override;
  void Delete(Vector &row_ids, idx_t count) override;
  void Fetch(Vector &row_ids, idx_t count, DataChunk &out) override;

  /**
   * @brief Free every heap page (the whole linked page chain) back to the buffer pool's free list.
   *
   * Walks `page_directory_` — the complete list of pages this heap ever linked — deleting each. The
   * heap is left empty (no pages, no first/last); callers must drop it right after (DROP TABLE).
   */
  void FreeAllPages() override;

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
  /**
   * Every page of the heap, in link order, so a parallel scan can address page *k* directly (a linked
   * list cannot). Appended under `latch_` whenever a new page is linked; the substrate for `ParallelScanState`.
   */
  std::vector<page_id_t> page_directory_;
};

}  // namespace bumblebee
