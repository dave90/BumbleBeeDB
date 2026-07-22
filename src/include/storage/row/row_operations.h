//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// row_operations.h
//
// Identification: src/include/storage/row/row_operations.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstring>
#include <vector>

#include "common/config.h"
#include "storage/row/row_layout.h"
#include "type/logical_type.h"
#include "type/vector/data_chunk.h"
#include "type/vector/selection_vector.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * @brief The SIMD-style bridge between column `Vector`s and row bytes in a page slot.
 *
 * These are the templated, physical-type-dispatched kernels (`Load<T>` / `Store<T>` in tight loops)
 * that move data between a `DataChunk` and rows addressed by a `Vector` of raw pointers — the same
 * mechanism BumbleBee uses for its hash tables, reused here for the row-format table so table access
 * is fully vectorized with no per-value boxing.
 */
struct RowOperations {
  /**
   * @brief Scatter a chunk's columns into pre-allocated rows.
   *
   * Each row pointer in `rows` must address a slot already sized to `layout.GetFixedRowWidth()` plus
   * this row's variable-length payload. Fixed columns are stored inline; a variable-length column's
   * payload is appended after the fixed region and referenced by an 8-byte `StringHandle`.
   *
   * @param columns The source chunk.
   * @param layout The row layout.
   * @param rows A flat `Vector` of `data_ptr_t` row pointers.
   * @param sel Selects which of `columns`' rows to scatter.
   * @param count The number of rows.
   */
  static void Scatter(DataChunk &columns, const RowLayout &layout, Vector &rows, const SelectionVector &sel,
                      idx_t count);

  /**
   * @brief Gather one column out of `count` rows into `col`, reading rows through identity order.
   *
   * A variable-length column is reconstructed as a `string_t` pointing into the row's payload (i.e.
   * into the pinned frame) — valid for as long as the caller keeps the page pinned.
   *
   * @param layout The row layout.
   * @param rows A flat `Vector` of `data_ptr_t` row pointers.
   * @param col The output column vector.
   * @param count The number of rows.
   * @param col_no The column to gather.
   * @param copy_strings When true, a string is copied into `col`'s own heap (so `col` outlives the
   *        rows); when false, it references the row bytes in place (valid while the page stays pinned).
   */
  static void FullScanColumn(const RowLayout &layout, Vector &rows, Vector &col, idx_t count, idx_t col_no,
                             bool copy_strings = false);

  /**
   * @brief Gather one column out of `count` rows into `col`, through explicit row/output selections.
   *
   * The i-th gathered value reads the row at `rows[row_sel[i]]` and lands at position `col_sel[i]` of
   * `col`. A **null row pointer** yields NULL — that is how a LEFT join NULL-pads the build columns of
   * an unmatched probe row without a separate emission path.
   *
   * @param layout The row layout.
   * @param rows A flat `Vector` of `data_ptr_t` row pointers (null = emit NULL).
   * @param row_sel Selects which row pointer feeds the i-th value.
   * @param col The output column vector (flat).
   * @param col_sel Selects which output position the i-th value lands at.
   * @param count The number of values.
   * @param col_no The layout column to gather.
   * @param copy_strings When true, strings are copied into `col`'s heap; when false they reference the
   *        row bytes in place (valid while the rows stay alive).
   */
  static void Gather(const RowLayout &layout, Vector &rows, const SelectionVector &row_sel, Vector &col,
                     const SelectionVector &col_sel, idx_t count, idx_t col_no, bool copy_strings = false);

  /**
   * @brief Vectorized key equality between chunk rows and scattered rows — the hash-table verify kernel.
   *
   * Candidate i compares columns row `col_sel[i]` of `columns` against the row bytes at
   * `rows[row_sel[i]]`, over the **first `key_count` layout columns** (the key prefix; any further
   * layout columns are payload and are not compared). Candidates are filtered column by column, so each
   * key column is one tight typed loop over the survivors of the previous column.
   *
   * On return `match_sel` holds the surviving candidate positions (in candidate order) and
   * `no_match_sel` the failing ones; the return value is the match count.
   *
   * @param columns The probe-side chunk holding the key columns first.
   * @param col_data `columns.Orrify()` output, computed once by the caller.
   * @param layout The row layout of the scattered rows.
   * @param key_count How many leading layout columns form the comparison key.
   * @param rows A flat `Vector` of `data_ptr_t` row pointers.
   * @param row_sel Maps candidate i to its row pointer slot.
   * @param col_sel Maps candidate i to its `columns` row.
   * @param count The number of candidates.
   * @param match_sel Out: surviving candidate positions.
   * @param no_match_sel Out: failing candidate positions.
   * @param no_match_count Out: how many candidates failed.
   * @param null_equal When true, two NULL keys compare equal (IS NOT DISTINCT FROM — GROUP BY
   *        semantics); when false a NULL key on either side never matches (SQL `=` — join semantics).
   */
  static auto Match(DataChunk &columns, VectorData col_data[], const RowLayout &layout, idx_t key_count,
                    Vector &rows, const SelectionVector &row_sel, const SelectionVector &col_sel, idx_t count,
                    SelectionVector &match_sel, SelectionVector &no_match_sel, idx_t &no_match_count,
                    bool null_equal) -> idx_t;

  /**
   * @brief Gather selected key columns from a chunk into a packed array of fixed-size index keys.
   *
   * The vectorized replacement for a per-cell `GetValue` + `Store` loop when building an index. For
   * each key column `k`, reads chunk column `src_cols[k]` and stores it — dispatched on `key_types[k]`
   * via the same templated `Store<T>` kernels as `Scatter` — at byte `dst_offsets[k]` of every key
   * slot. Key slots are `key_stride` bytes apart in `out`, which the caller pre-zeroes for padding.
   *
   * Index keys carry no validity prefix and only fixed-width columns are supported (a variable-length
   * key type throws), matching the `GenericComparator`.
   *
   * @param chunk The source chunk (base-table columns).
   * @param src_cols Which chunk column feeds each key column.
   * @param dst_offsets The byte offset of each key column within a key slot (from the key schema).
   * @param key_types The physical type of each key column.
   * @param out The packed output buffer of `count * key_stride` bytes.
   * @param key_stride The size of one key slot in bytes.
   * @param count The number of keys (rows) to gather.
   */
  static void ScatterKeys(DataChunk &chunk, const std::vector<uint32_t> &src_cols,
                          const std::vector<idx_t> &dst_offsets, const std::vector<PhysicalType> &key_types,
                          data_ptr_t out, size_t key_stride, idx_t count);
};

// --- per-row validity prefix helpers (row offset 0; bit = 1 means valid) -----------------------

/** @brief Mark every column of a row valid (set the whole prefix to 0xFF). */
inline void RowSetAllValid(data_ptr_t row, idx_t flag_width) {
  if (flag_width != 0) {
    std::memset(row, 0xFF, flag_width);
  }
}

/** @brief Clear the validity bit for column `col_no` (mark it NULL). */
inline void RowSetInvalid(data_ptr_t row, idx_t col_no) { row[col_no >> 3] &= ~(uint8_t(1) << (col_no & 7)); }

/** @return True if column `col_no` of this row is valid (not NULL). */
inline auto RowIsValid(const_data_ptr_t row, idx_t col_no) -> bool {
  return (row[col_no >> 3] & (uint8_t(1) << (col_no & 7))) != 0;
}

}  // namespace bumblebee
