//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// chunk_collection.h
//
// Identification: src/include/type/vector/chunk_collection.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/**
 * A materialized relation: an ordered list of DataChunks, all of the same types.
 *
 * This is what the operators that have to see all of their input at once (sort, hash join
 * build side, a result set) accumulate into. Every chunk but the last is full, so row `i`
 * always lives in chunk `i / STANDARD_VECTOR_SIZE`.
 */
class ChunkCollection {
 public:
  ChunkCollection() = default;

  /** @return The type of every column. */
  auto GetTypes() -> std::vector<LogicalType> { return types_; }

  /** @return The number of rows in the collection. */
  auto GetCount() const -> const idx_t & { return count_; }

  /** @return The number of columns in the collection. */
  auto ColumnCount() const -> idx_t { return types_.size(); }

  /** @brief Append a chunk, filling up the last chunk first. COPIES the data. */
  void Append(DataChunk &chunk);

  /** @brief Take ownership of `chunk` and append it as-is. Does NOT copy. */
  void Append(data_chunk_ptr_t chunk);

  /** @brief Append every chunk of `other`. */
  void Append(ChunkCollection &other);

  /** @brief Like Append, but it reorders the chunks and empties `other`. */
  void Merge(ChunkCollection &other);

  /** @brief Add the columns of `other` to the right of this collection's. */
  void Fuse(ChunkCollection &other);

  /** @brief Exchange two chunks. */
  void SwapChunks(idx_t index1, idx_t index2);

  /** @return The value of `column` at row `index`. */
  auto GetValue(idx_t column, idx_t index) -> Value;

  /** @brief Write `value` into `column` at row `index`. */
  void SetValue(idx_t column, idx_t index, const Value &value);

  /** @brief Copy one cell into `target` at `target_offset`. */
  void CopyCell(idx_t column, idx_t index, Vector &target, idx_t target_offset);

  /** @return A printable rendering of the collection. */
  auto ToString() const -> std::string;

  /** @return The chunk holding row `row_index`. */
  auto GetChunkForRow(idx_t row_index) -> DataChunk & { return *chunks_[LocateChunk(row_index)]; }

  /** @return The chunk at `chunk_index`. */
  auto GetChunk(idx_t chunk_index) -> DataChunk & {
    BUMBLEBEE_ASSERT(chunk_index < chunks_.size(), "ChunkCollection::GetChunk: the index is out of range");
    return *chunks_[chunk_index];
  }

  /** @return The chunk at `chunk_index`. */
  auto GetChunk(idx_t chunk_index) const -> const DataChunk & {
    BUMBLEBEE_ASSERT(chunk_index < chunks_.size(), "ChunkCollection::GetChunk: the index is out of range");
    return *chunks_[chunk_index];
  }

  /** @return The chunks. */
  auto Chunks() -> std::vector<data_chunk_ptr_t> & { return chunks_; }

  /** @return The number of chunks. */
  auto ChunkCount() const -> idx_t { return chunks_.size(); }

  /** @brief Drop every chunk. */
  void Reset() {
    count_ = 0;
    chunks_.clear();
    types_.clear();
  }

  /** @return The first chunk, removed from the collection. Null when there is none. */
  auto Fetch() -> data_chunk_ptr_t {
    if (ChunkCount() == 0) {
      return nullptr;
    }
    auto res = std::move(chunks_[0]);
    count_ -= res->GetSize();
    chunks_.erase(chunks_.begin());
    return res;
  }

  /**
   * @brief True if the two collections hold the same rows in the same order.
   *
   * NOT vectorized: this reads value by value. Do not use it during execution.
   */
  auto Equals(ChunkCollection &other) -> bool;

  /** @return The index of the chunk holding row `index`. */
  auto LocateChunk(idx_t index) -> idx_t {
    idx_t result = index / STANDARD_VECTOR_SIZE;
    BUMBLEBEE_ASSERT(result < chunks_.size(), "ChunkCollection::LocateChunk: the row is out of range");
    return result;
  }

  /** @brief Cast every chunk to `new_types`. */
  void Cast(const std::vector<LogicalType> &new_types) {
    types_ = new_types;
    if (count_ == 0) {
      return;
    }
    for (auto &chunk : chunks_) {
      chunk->Cast(new_types);
    }
  }

 private:
  /** The number of rows in the collection. */
  idx_t count_{0};
  /** The chunks, in order. */
  std::vector<data_chunk_ptr_t> chunks_;
  /** The type of every column. */
  std::vector<LogicalType> types_;
};

}  // namespace bumblebee
