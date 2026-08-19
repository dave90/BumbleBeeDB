//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_storage.h
//
// Identification: src/include/storage/table/table_storage.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/config.h"
#include "common/macros.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** The physical layout a table's rows are stored in. */
enum class StorageFormat : uint8_t { ROW = 0, PARQUET = 1 };

/**
 * @brief A forward, vectorized scan over a table.
 *
 * Every backend yields `DataChunk`s of up to `STANDARD_VECTOR_SIZE` live rows (deleted rows are
 * skipped). A row backend gathers slotted-page rows into column vectors; a columnar backend reads
 * row-group chunks natively.
 *
 * Contract: the chunk a `Next` returns is valid until the following `Next` call — a row backend may
 * keep the page pinned across the return so string values can reference the frame zero-copy.
 */
class TableScan {
 public:
  virtual ~TableScan() = default;

  /**
   * @brief Fill `out` with the next batch of rows.
   *
   * @param out A chunk pre-initialized to the projected column types.
   * @param row_ids When non-null, receives each row's identifier (a `BIGINT`/`ADDRESS` vector).
   * @return true if a batch was produced, false at end of table.
   */
  virtual auto Next(DataChunk &out, Vector *row_ids = nullptr) -> bool = 0;
};

/**
 * @brief The physical storage behind a table, accessed entirely in vectorized form.
 *
 * A row backend (`TableHeap`) is mutable; a columnar backend (`ParquetTable`) is read-only and throws
 * `NotImplementedException` from the mutating methods.
 */
class TableStorage {
 public:
  TableStorage() = default;
  virtual ~TableStorage() = default;
  TableStorage(const TableStorage &) = delete;
  auto operator=(const TableStorage &) -> TableStorage & = delete;
  TableStorage(TableStorage &&) = delete;
  auto operator=(TableStorage &&) -> TableStorage & = delete;

  virtual auto GetFormat() const -> StorageFormat = 0;

  /**
   * @brief A cheap estimate of the number of rows this storage holds, for cost-based planning.
   *
   * Approximate by design (a heap counts slotted-page tuples including logically-deleted ones; a
   * parquet table sums its manifest's per-file row counts). Returns 0 when unknown — the caller
   * (the optimizer's cardinality estimator) treats 0 as "no signal" and falls back to a default.
   */
  virtual auto EstimatedRowCount() const -> idx_t { return 0; }

  /**
   * @brief Open a scan.
   *
   * @param projection Column indices to materialize (column pruning); empty means all columns.
   */
  virtual auto MakeScan(const std::vector<idx_t> &projection = {}) -> std::unique_ptr<TableScan> = 0;

  /** @brief Append a chunk of rows; writes each new row's identifier into `out_rids`. */
  virtual void Append(DataChunk &chunk, Vector &out_rids) = 0;

  /** @brief Update the `chunk.GetSize()` rows named by `row_ids` with `chunk`; rewrites `row_ids`. */
  virtual void Update(Vector &row_ids, DataChunk &chunk) = 0;

  /** @brief Logically delete the first `count` rows named by `row_ids`. */
  virtual void Delete(Vector &row_ids, idx_t count) = 0;

  /** @brief Gather the first `count` rows named by `row_ids` into `out` (a self-contained chunk). */
  virtual void Fetch(Vector &row_ids, idx_t count, DataChunk &out) = 0;

  /**
   * @brief Return every page this storage owns to the buffer pool's free list (DROP TABLE).
   *
   * After this call the storage is empty and must not be used again. The default is a no-op — a
   * backend with no reclaimable pages of its own (e.g. an external columnar file) needs nothing here.
   */
  virtual void FreeAllPages() {}
};

}  // namespace bumblebee
