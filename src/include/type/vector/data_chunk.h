//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// data_chunk.h
//
// Identification: src/include/type/vector/data_chunk.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "type/value.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * A horizontal slice of a relation: a set of equal-length Vectors, one per column.
 *
 * DataChunk is the unit of data every operator passes to the next. Its vectors may own
 * their data or merely reference another chunk's — a Filter, for instance, does not copy
 * anything, it just puts a selection on top of the vectors it was given.
 */
class DataChunk {
 public:
  /** The columns. Public by design: the operators index into them directly. */
  std::vector<Vector> data_;

  DataChunk();
  DataChunk(DataChunk &&other) noexcept;
  DataChunk(const DataChunk &) = delete;
  auto operator=(const DataChunk &) -> DataChunk & = delete;
  auto operator=(DataChunk &&) -> DataChunk & = delete;
  ~DataChunk() = default;

  /** @return The number of rows currently held. */
  auto GetSize() const -> idx_t { return count_; }

  /** @return The number of columns. */
  auto ColumnCount() const -> idx_t { return data_.size(); }

  /** @brief Set the number of rows. Must not exceed the capacity. */
  void SetCardinality(idx_t count) {
    BUMBLEBEE_ASSERT(count <= capacity_, "DataChunk::SetCardinality: the count exceeds the capacity");
    count_ = count;
  }

  /** @brief Take the row count of `other`. */
  void SetCardinality(const DataChunk &other) { count_ = other.GetSize(); }

  /** @brief Take the capacity of `other`. */
  void SetCapacity(const DataChunk &other) { capacity_ = other.capacity_; }

  /** @brief Set the number of rows the chunk can hold. */
  void SetCapacity(idx_t capacity) { capacity_ = capacity; }

  /** @return The number of rows the chunk can hold. */
  auto GetCapacity() const -> idx_t { return capacity_; }

  /** @return The value of column `col` at row `index`. */
  auto GetValue(idx_t col, idx_t index) const -> Value;

  /** @brief Write `val` into column `col` at row `index`. */
  void SetValue(idx_t col, idx_t index, const Value &val);

  // -- Referencing ----------------------------------------------------------

  /** @brief Make every column reference the matching column of `chunk`. */
  void Reference(const DataChunk &chunk);

  /** @brief Reference only the columns of `chunk` named by `cols`, in that order. */
  void Reference(DataChunk &chunk, const std::vector<idx_t> &cols);

  /** @brief InitializeEmpty with the types of `chunk`'s `cols`, then reference them. */
  void InitAndReference(DataChunk &chunk, const std::vector<idx_t> &cols);

  /** @brief InitializeEmpty with the types of `chunk`, then reference it. */
  void InitAndReference(DataChunk &chunk);

  /** @return A new chunk referencing this one. */
  auto Clone() -> std::unique_ptr<DataChunk>;

  // -- Initialization -------------------------------------------------------

  /** @brief Create one owned, allocated Vector per type. */
  void Initialize(const std::vector<PhysicalType> &types);

  /** @brief Create one owned, allocated Vector per type. */
  void Initialize(const std::vector<LogicalType> &types);

  /** @brief Allocate only the columns named by `cols_to_initialize`; the rest stay empty. */
  void Initialize(const std::vector<LogicalType> &types, const std::unordered_set<idx_t> &cols_to_initialize);

  /** @brief Create one Vector per type, with NO data allocated: they are meant to reference. */
  void InitializeEmpty(const std::vector<PhysicalType> &types);

  /** @brief Create one Vector per type, with NO data allocated: they are meant to reference. */
  void InitializeEmpty(const std::vector<LogicalType> &types);

  // -- Data movement --------------------------------------------------------

  /**
   * @brief Append the rows of `other` to this chunk.
   *
   * The column count and types have to match exactly.
   *
   * @param other The chunk to append.
   * @param resize Whether growing past the capacity is allowed.
   * @param sel The rows of `other` to take. Null means all of them.
   * @param count The number of rows to take when `sel` is given.
   */
  void Append(const DataChunk &other, bool resize = false, SelectionVector *sel = nullptr, idx_t count = 0);

  /** @brief Grow every column to `size` rows. */
  void Resize(idx_t size);

  /** @brief Drop every column and its data. */
  void Destroy();

  /** @brief Copy rows [offset, GetSize()) of this chunk into `other`. */
  void Copy(DataChunk &other, idx_t offset = 0) const;

  /** @brief Copy the rows of this chunk named by `sel` into `other`. */
  void Copy(DataChunk &other, const SelectionVector &sel, idx_t source_count, idx_t offset = 0) const;

  /** @brief Move the columns from `split_index` on into `other`. */
  void Split(DataChunk &other, idx_t split_index);

  /** @brief Turn every column into a FLAT_VECTOR. */
  void Normalify();

  /** @return The (data, selection, validity) triple of every column. */
  auto Orrify() -> array_vector_data_t;

  /** @brief Put `sel` on top of every column. */
  void Slice(const SelectionVector &sel_vector, idx_t count);

  /** @brief Slice the columns of `other` into this chunk, starting at column `col_offset`. */
  void Slice(DataChunk &other, const SelectionVector &sel, idx_t count, idx_t col_offset = 0);

  /** @brief Slice the columns of `other` into the columns of this chunk named by `cols_map`. */
  void Slice(DataChunk &other, const SelectionVector &sel, idx_t count, const std::vector<idx_t> &cols_map);

  /** @brief Back to the state right after Initialize: no rows, every vector owning its data again. */
  void Reset();

  /** @brief Reset, re-allocating only the columns named by `columns_to_reset`. */
  void Reset(const std::vector<idx_t> &columns_to_reset);

  /** @return The type of every column. */
  auto GetTypes() const -> std::vector<LogicalType>;

  /**
   * @return An estimate of the bytes these rows occupy once materialized: the inline
   *         stride of every column plus the out-of-line payloads (string bytes, list and
   *         array elements) the inline stride only points at.
   */
  auto EstimatedBytes() -> idx_t;

  /** @return A printable rendering of the chunk. */
  auto ToString() const -> std::string;

  // -- Kernels --------------------------------------------------------------

  /** @brief Hash every column of every row into `result` (a UBIGINT vector). */
  void Hash(Vector &result);

  /** @brief Hash the columns named by `cols` into `result`. */
  void Hash(Vector &result, const std::vector<idx_t> &cols);

  /** @brief Cast every column to the matching type. COPIES the data. */
  void Cast(const std::vector<LogicalType> &types);

  /** @brief Cast every column into the matching (already typed) column of `result`. */
  void Cast(DataChunk &result);

 private:
  /** @brief Remember each column's freshly-allocated buffer for reuse by Reset(). */
  void CacheBuffers();

  /** The number of rows held. */
  idx_t count_;
  /** The number of rows that fit. */
  idx_t capacity_;
  /** Each column's originally-allocated flat buffer (and its physical type), kept so Reset()
   * can re-point the column at it instead of freeing + re-allocating every buffer on every
   * chunk iteration. A buffer still referenced by someone else is left to them and replaced
   * with a fresh allocation. Empty (e.g. after InitializeEmpty) until the first Reset(). */
  std::vector<PhysicalType> cache_types_;
  std::vector<vector_data_mngr_ptr_t> cache_mngrs_;
};

using data_chunk_ptr_t = std::unique_ptr<DataChunk>;
using data_chunk_vector_t = std::vector<data_chunk_ptr_t>;

}  // namespace bumblebee
