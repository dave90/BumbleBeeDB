//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// chunk_collection.cpp
//
// Identification: src/type/vector/chunk_collection.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/chunk_collection.h"

#include <algorithm>
#include <utility>

#include "common/exception.h"
#include "common/helper.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

void ChunkCollection::Append(DataChunk &chunk) {
  if (chunk.GetSize() == 0) {
    return;
  }
  count_ += chunk.GetSize();

  idx_t data_to_append = chunk.GetSize();
  idx_t offset = 0;
  if (chunks_.empty()) {
    types_ = chunk.GetTypes();
  } else {
    auto types = chunk.GetTypes();
    BUMBLEBEE_ASSERT(types.size() == types_.size(), "ChunkCollection::Append: the column counts do not match");
    for (idx_t i = 0; i < types_.size(); ++i) {
      if (types[i] != types_[i]) {
        throw Exception(ExceptionType::MISMATCH_TYPE,
                        fmt::format("ChunkCollection::Append: cannot append a {} column to a {} column",
                                    types[i].ToString(), types_[i].ToString()));
      }
    }
    // Fill up the last chunk before starting a new one.
    auto &last_chunk = chunks_.back();
    auto available_size = MinValue(data_to_append, STANDARD_VECTOR_SIZE - last_chunk->GetSize());
    if (available_size > 0) {
      chunk.Normalify();
      auto old_count = chunk.GetSize();
      // Append only as many rows as fit, by lying about the source's cardinality.
      chunk.SetCardinality(available_size);
      last_chunk->Append(chunk);
      data_to_append -= available_size;
      chunk.SetCardinality(old_count);
      offset = available_size;
    }
  }

  if (data_to_append > 0) {
    auto chunk_to_add = data_chunk_ptr_t(new DataChunk());
    chunk_to_add->Initialize(types_);
    chunk.Copy(*chunk_to_add, offset);
    chunks_.push_back(std::move(chunk_to_add));
  }
}

void ChunkCollection::Append(data_chunk_ptr_t chunk) {
  if (chunks_.empty()) {
    types_ = chunk->GetTypes();
  }
  count_ += chunk->GetSize();
  chunks_.push_back(std::move(chunk));
}

void ChunkCollection::Append(ChunkCollection &other) {
  for (auto &chunk : other.chunks_) {
    Append(*chunk);
  }
}

void ChunkCollection::Merge(ChunkCollection &other) {
  if (other.count_ == 0) {
    return;
  }
  if (count_ == 0) {
    count_ = other.count_;
    types_ = std::move(other.types_);
    chunks_ = std::move(other.chunks_);
    return;
  }
  BUMBLEBEE_ASSERT(types_ == other.types_, "ChunkCollection::Merge: the types do not match");
  data_chunk_ptr_t last_chunk;
  if (chunks_.back()->GetSize() != STANDARD_VECTOR_SIZE) {
    // Our last chunk is not full: take it out, and append it again at the end so that the
    // gap gets filled instead of leaving a hole in the middle.
    last_chunk = std::move(chunks_.back());
    count_ -= last_chunk->GetSize();
    chunks_.pop_back();
  }
  for (auto &chunk : other.chunks_) {
    Append(std::move(chunk));
  }
  if (last_chunk) {
    Append(*last_chunk);
  }
}

void ChunkCollection::Fuse(ChunkCollection &other) {
  types_.insert(types_.end(), other.types_.begin(), other.types_.end());
  if (count_ == 0) {
    // This collection is empty: build empty chunks that reference the other's vectors.
    chunks_.reserve(other.ChunkCount());
    for (idx_t i = 0; i < other.ChunkCount(); ++i) {
      auto lhs = data_chunk_ptr_t(new DataChunk());
      auto &rhs = other.GetChunk(i);
      lhs->data_.reserve(rhs.data_.size());
      for (auto &v : rhs.data_) {
        lhs->data_.emplace_back(Vector(v));
      }
      lhs->SetCardinality(rhs.GetSize());
      chunks_.push_back(std::move(lhs));
    }
    count_ = other.count_;
    return;
  }
  BUMBLEBEE_ASSERT(count_ == other.count_, "ChunkCollection::Fuse: the row counts do not match");
  for (idx_t i = 0; i < other.ChunkCount(); ++i) {
    auto &lhs = chunks_[i];
    auto &rhs = other.GetChunk(i);
    BUMBLEBEE_ASSERT(lhs->GetSize() == rhs.GetSize(), "ChunkCollection::Fuse: the chunk sizes do not match");
    for (auto &v : rhs.data_) {
      lhs->data_.emplace_back(Vector(v));
    }
  }
}

void ChunkCollection::SwapChunks(idx_t index1, idx_t index2) { std::swap(chunks_[index1], chunks_[index2]); }

void ChunkCollection::Verify() {
  // TODO(milestone-2): a DEBUG-only structural check of the collection.
}

auto ChunkCollection::GetValue(idx_t column, idx_t index) -> Value {
  return GetChunkForRow(index).GetValue(column, index % STANDARD_VECTOR_SIZE);
}

void ChunkCollection::SetValue(idx_t column, idx_t index, const Value &value) {
  GetChunkForRow(index).SetValue(column, index % STANDARD_VECTOR_SIZE, value);
}

void ChunkCollection::CopyCell(idx_t column, idx_t index, Vector &target, idx_t target_offset) {
  auto &source = GetChunkForRow(index).data_[column];
  auto source_offset = index % STANDARD_VECTOR_SIZE;
  VectorOperations::Copy(source, target, source_offset + 1, source_offset, target_offset);
}

auto ChunkCollection::ToString() const -> std::string {
  std::string result =
      chunks_.empty() ? "ChunkCollection [ 0 ]" : "ChunkCollection [ " + std::to_string(count_) + " ]: \n";
  for (const auto &chunk : chunks_) {
    result += chunk->ToString() + "\n";
  }
  return result;
}

auto ChunkCollection::Equals(ChunkCollection &other) -> bool {
  if (count_ != other.count_) {
    return false;
  }
  if (ColumnCount() != other.ColumnCount()) {
    return false;
  }
  if (types_ != other.types_) {
    return false;
  }
  // The row counts match, so the chunk counts do too.
  for (idx_t row = 0; row < count_; row++) {
    for (idx_t col = 0; col < ColumnCount(); col++) {
      if (GetValue(col, row) != other.GetValue(col, row)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace bumblebee
