//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// prl_hash_table.h
//
// Identification: src/include/execution/prl_hash_table.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "common/config.h"
#include "storage/row/row_layout.h"
#include "storage/row/row_operations.h"
#include "type/vector/data_chunk.h"
#include "type/vector/selection_vector.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** One directory slot: the row's full 64-bit hash and its address. `row_ == nullptr` means empty. */
struct HTEntry {
  hash_t hash_{0};
  data_ptr_t row_{nullptr};
};

/**
 * @brief Probe Row Layout hash table — the vectorized engine of the hash join and the hash aggregate.
 *
 * Rows are packed as `RowLayout` bytes into bump-allocated blocks (variable-length payloads inline, so
 * rows are addressed by pointer, never by stride); a separate open-addressed directory of `HTEntry`
 * maps hash buckets to row addresses with linear probing. All operations are chunk-at-a-time:
 *
 * - `FindOrCreateGroups` (group mode, GROUP BY / DISTINCT): per input chunk, classify every row against
 *   the directory in rounds — empty bucket → create the group (one batched `RowOperations::Scatter`
 *   for all new rows), equal hash → verify with one vectorized `RowOperations::Match` over the key
 *   prefix, mismatch → advance to the next bucket. Two NULL keys compare equal when the table is
 *   built with `null_equal_keys` (IS NOT DISTINCT FROM — SQL GROUP BY semantics).
 * - `Append` (join mode): insert every selected row unconditionally — SQL bag semantics; duplicate
 *   keys each keep their own entry, clustered by linear probing.
 * - `Probe`: collect every directory entry whose hash equals a probe row's hash by walking its
 *   cluster, then verify candidates in `STANDARD_VECTOR_SIZE` batches with `RowOperations::Match`.
 *   Emits (row address, probe row) pairs in probe-row order.
 * - `Scan`: read the rows back in insertion order, one vectorized gather per column.
 *
 * The first `key_count` layout columns are the comparison key; any further columns are payload
 * (carried, never compared).
 *
 * Not thread-safe; each task builds its own table and merges under the operator's lock.
 */
class PRLHashTable {
 public:
  /** The directory load factor beyond which the table doubles. */
  static constexpr float LOAD_FACTOR = 0.75;
  /** The initial directory capacity (a power of two). */
  static constexpr idx_t INITIAL_CAPACITY = 1024;
  /** The default payload block size in bytes (a single larger row gets a dedicated block). */
  static constexpr idx_t BLOCK_SIZE = 256 * 1024;

  /**
   * @param types The layout columns: the `key_count` key columns first, then the payload columns.
   * @param key_count How many leading columns form the comparison key.
   * @param null_equal_keys True for GROUP BY semantics (NULL == NULL), false for join semantics.
   * @param initial_capacity Directory capacity to start from (rounded up to a power of two).
   */
  PRLHashTable(std::vector<LogicalType> types, idx_t key_count, bool null_equal_keys,
               idx_t initial_capacity = INITIAL_CAPACITY);

  PRLHashTable(const PRLHashTable &) = delete;
  auto operator=(const PRLHashTable &) -> PRLHashTable & = delete;

  /**
   * @brief The selection of rows whose every key column is non-NULL (a NULL key never equi-matches).
   *
   * The shared probe/build-side filter of the equi-joins: SQL `=` treats `NULL = NULL` as unknown, so a
   * NULL-keyed row is dropped from the build table and (for INNER) from the probe.
   */
  static auto NonNullKeyRows(DataChunk &key_chunk, SelectionVector &sel) -> idx_t;

  /**
   * @brief Group mode: find each row's group, creating missing groups. Bag-deduplicating.
   *
   * @param hashes The key hash of every row of `groups` (a flat UBIGINT vector).
   * @param groups The full layout chunk (key columns first, then payload columns).
   * @param addresses Out: the group row address of every input row (flat `data_ptr_t` vector,
   *        capacity >= groups.GetSize()).
   * @param new_group_sel Out (optional): the input rows that created a new group, in creation order.
   * @param new_group_count Out (optional): how many groups were created.
   */
  void FindOrCreateGroups(Vector &hashes, DataChunk &groups, Vector &addresses,
                          SelectionVector *new_group_sel = nullptr, idx_t *new_group_count = nullptr);

  /**
   * @brief Join mode: append the selected rows unconditionally (no key deduplication).
   *
   * @param hashes The key hash of every row of `rows`, indexed by input row (flat UBIGINT vector).
   * @param rows The full layout chunk (key columns first, then payload columns).
   * @param sel Which input rows to append.
   * @param count How many.
   */
  void Append(Vector &hashes, DataChunk &rows, const SelectionVector &sel, idx_t count);

  /**
   * @brief Join mode, parallel-build phase: append rows WITHOUT touching the directory.
   *
   * This is the thread-local half of the DuckDB-style build: each sink task scatters its rows into
   * its own table's blocks (keys hashed, rows materialized), the tables are then spliced together
   * with `Merge`, and one `BuildDirectory` pass links every row in. The directory is stale after
   * this call — `Probe`/`FindOrCreateGroups` assert against it until `BuildDirectory` runs.
   */
  void AppendUnbuilt(Vector &hashes, DataChunk &rows, const SelectionVector &sel, idx_t count);

  /**
   * @brief Splice `other`'s rows into this table: O(blocks), rows never move or re-scatter.
   *
   * Steals `other`'s payload blocks, row addresses and stored hashes (types must match). The
   * directory becomes stale; call `BuildDirectory` once after the last merge. `other` is left empty.
   */
  void Merge(PRLHashTable &other);

  /**
   * @brief (Re)build the directory over every row, sized once from the final count.
   *
   * One pass over the stored (hash, address) pairs — key bytes are never touched. Clears the
   * stale-directory state set by `AppendUnbuilt`/`Merge`.
   */
  void BuildDirectory();

  /**
   * @brief Find every verified match of the selected probe rows.
   *
   * Appends one (row address, probe row) pair per match to `out_addrs`/`out_rows`, in probe-row order
   * (all matches of one probe row are contiguous). A probe row with a NULL key matches nothing unless
   * the table was built with `null_equal_keys`.
   *
   * @param hashes The key hash of every probe row, indexed by probe row (flat UBIGINT vector).
   * @param keys The probe key chunk — exactly the `key_count` key columns.
   * @param sel Which probe rows to look up.
   * @param count How many.
   * @param out_addrs Out: the matched build-row addresses (appended).
   * @param out_rows Out: the matching probe row of each address (appended).
   * @param matched Out (optional): flags indexed by probe row, set to 1 when the row matched anything.
   */
  void Probe(Vector &hashes, DataChunk &keys, const SelectionVector &sel, idx_t count,
             std::vector<data_ptr_t> &out_addrs, std::vector<sel_t> &out_rows,
             std::vector<uint8_t> *matched = nullptr);

  /**
   * @brief Read rows back in insertion order: fills the first `result.ColumnCount()` layout columns.
   *
   * @param offset The first row to read.
   * @param result The output chunk (flat, capacity >= STANDARD_VECTOR_SIZE).
   * @param copy_strings When true, strings are copied into the result's heaps (so the result outlives
   *        this table); when false they reference the row bytes in place.
   * @return The number of rows produced (0 when `offset` is past the end).
   */
  auto Scan(idx_t offset, DataChunk &result, bool copy_strings = false) -> idx_t;

  /** @return The number of rows in the table. */
  auto Count() const -> idx_t { return count_; }

  /** @return The row layout (key columns first, then payload columns). */
  auto GetLayout() const -> const RowLayout & { return layout_; }

  /** @return The layout column types. */
  auto GetTypes() const -> const std::vector<LogicalType> & { return types_; }

 private:
  /** @brief Grow the directory to `new_capacity` (a power of two) and rehash every entry. */
  void Resize(idx_t new_capacity);

  /** @brief Bump-allocate `size` bytes for one row. */
  auto AllocateRow(idx_t size) -> data_ptr_t;

  /** @brief The byte size of every row of `chunk`: the fixed width plus its varlen payloads. */
  auto ComputeRowSizes(DataChunk &chunk) const -> std::vector<uint32_t>;

  std::vector<LogicalType> types_;
  idx_t key_count_;
  bool null_equal_keys_;
  RowLayout layout_;

  idx_t capacity_{0};
  hash_t bitmask_{0};
  idx_t count_{0};
  std::vector<HTEntry> directory_;

  /** Payload blocks (bump-allocated); rows never move once written. */
  std::vector<std::unique_ptr<data_t[]>> blocks_;
  idx_t block_used_{0};
  idx_t block_capacity_{0};

  /** Every row address in insertion order — the Scan cursor's backing. */
  std::vector<data_ptr_t> row_addrs_;
  /** Every row's key hash, parallel to `row_addrs_` — lets `BuildDirectory` run without key bytes. */
  std::vector<hash_t> row_hashes_;
  /** True while rows appended by `AppendUnbuilt`/`Merge` are missing from the directory. */
  bool directory_stale_{false};
};

}  // namespace bumblebee
