//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// data_chunk.cpp
//
// Identification: src/type/vector/data_chunk.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/data_chunk.h"

#include <unordered_set>
#include <utility>

#include "common/exception.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

DataChunk::DataChunk() : count_(0), capacity_(STANDARD_VECTOR_SIZE) {}

DataChunk::DataChunk(DataChunk &&other) noexcept
    : data_(std::move(other.data_)),
      count_(other.count_),
      capacity_(other.capacity_),
      cache_types_(std::move(other.cache_types_)),
      cache_mngrs_(std::move(other.cache_mngrs_)) {
  other.Destroy();
}

void DataChunk::CacheBuffers() {
  cache_types_.clear();
  cache_mngrs_.clear();
  cache_types_.reserve(data_.size());
  cache_mngrs_.reserve(data_.size());
  for (auto &v : data_) {
    cache_types_.push_back(v.GetType());
    cache_mngrs_.push_back(v.GetDataMngr());
  }
}

auto DataChunk::GetValue(idx_t col, idx_t index) const -> Value {
  BUMBLEBEE_ASSERT(col < ColumnCount(), "DataChunk::GetValue: the column is out of range");
  BUMBLEBEE_ASSERT(index < GetSize(), "DataChunk::GetValue: the row is out of range");
  return data_[col].GetValue(index);
}

void DataChunk::SetValue(idx_t col, idx_t index, const Value &val) {
  BUMBLEBEE_ASSERT(col < ColumnCount(), "DataChunk::SetValue: the column is out of range");
  BUMBLEBEE_ASSERT(index < GetCapacity(), "DataChunk::SetValue: the row is out of range");
  data_[col].SetValue(index, val);
}

void DataChunk::Reference(const DataChunk &chunk) {
  BUMBLEBEE_ASSERT(ColumnCount() >= chunk.ColumnCount(), "DataChunk::Reference: not enough columns");
  SetCapacity(chunk);
  SetCardinality(chunk);
  for (idx_t i = 0; i < chunk.ColumnCount(); ++i) {
    data_[i].Reference(chunk.data_[i]);
  }
}

void DataChunk::Reference(DataChunk &chunk, const std::vector<idx_t> &cols) {
  BUMBLEBEE_ASSERT(ColumnCount() >= cols.size(), "DataChunk::Reference: not enough columns");
  SetCapacity(chunk);
  SetCardinality(chunk);
  for (idx_t i = 0; i < cols.size(); ++i) {
    auto idx = cols[i];
    BUMBLEBEE_ASSERT(idx < chunk.ColumnCount(), "DataChunk::Reference: the column is out of range");
    data_[i].Reference(chunk.data_[idx]);
  }
}

void DataChunk::InitAndReference(DataChunk &chunk, const std::vector<idx_t> &cols) {
  auto chunk_types = chunk.GetTypes();
  std::vector<LogicalType> types;
  types.reserve(cols.size());
  for (idx_t i = 0; i < cols.size(); ++i) {
    BUMBLEBEE_ASSERT(i < chunk_types.size(), "DataChunk::InitAndReference: the column is out of range");
    types.push_back(chunk_types[i]);
  }
  InitializeEmpty(types);
  Reference(chunk, cols);
}

void DataChunk::InitAndReference(DataChunk &chunk) {
  InitializeEmpty(chunk.GetTypes());
  Reference(chunk);
}

auto DataChunk::Clone() -> std::unique_ptr<DataChunk> {
  data_chunk_ptr_t chunk = std::make_unique<DataChunk>();
  chunk->InitializeEmpty(GetTypes());
  chunk->Reference(*this);
  return chunk;
}

void DataChunk::Initialize(const std::vector<PhysicalType> &types) {
  BUMBLEBEE_ASSERT(data_.empty(), "DataChunk::Initialize: the chunk already has columns");
  capacity_ = STANDARD_VECTOR_SIZE;
  data_.reserve(types.size());
  for (const auto &type : types) {
    data_.emplace_back(type);
  }
  CacheBuffers();
}

void DataChunk::Initialize(const std::vector<LogicalType> &types) {
  BUMBLEBEE_ASSERT(data_.empty(), "DataChunk::Initialize: the chunk already has columns");
  capacity_ = STANDARD_VECTOR_SIZE;
  data_.reserve(types.size());
  for (const auto &type : types) {
    data_.emplace_back(type);
  }
  CacheBuffers();
}

void DataChunk::Initialize(const std::vector<LogicalType> &types, const std::unordered_set<idx_t> &cols_to_initialize) {
  BUMBLEBEE_ASSERT(data_.empty(), "DataChunk::Initialize: the chunk already has columns");
  capacity_ = STANDARD_VECTOR_SIZE;
  data_.reserve(types.size());
  for (idx_t i = 0; i < types.size(); ++i) {
    if (!cols_to_initialize.contains(i)) {
      data_.emplace_back(types[i], nullptr);
    } else {
      data_.emplace_back(types[i]);
    }
  }
  CacheBuffers();
}

void DataChunk::InitializeEmpty(const std::vector<PhysicalType> &types) {
  BUMBLEBEE_ASSERT(data_.empty(), "DataChunk::InitializeEmpty: the chunk already has columns");
  capacity_ = STANDARD_VECTOR_SIZE;
  data_.reserve(types.size());
  for (const auto &type : types) {
    data_.emplace_back(type, nullptr);
  }
}

void DataChunk::InitializeEmpty(const std::vector<LogicalType> &types) {
  BUMBLEBEE_ASSERT(data_.empty(), "DataChunk::InitializeEmpty: the chunk already has columns");
  capacity_ = STANDARD_VECTOR_SIZE;
  data_.reserve(types.size());
  for (const auto &type : types) {
    data_.emplace_back(type, nullptr);
  }
}

void DataChunk::Append(const DataChunk &other, bool resize, SelectionVector *sel, idx_t count) {
  if (other.GetSize() == 0) {
    return;
  }
  idx_t new_size = sel != nullptr ? GetSize() + count : GetSize() + other.GetSize();
  BUMBLEBEE_ASSERT(ColumnCount() == other.ColumnCount(), "DataChunk::Append: the column counts do not match");

  if (new_size > capacity_) {
    BUMBLEBEE_ASSERT(resize, "DataChunk::Append: the chunk does not fit and resizing was not allowed");
    (void)resize;
    for (idx_t i = 0; i < ColumnCount(); i++) {
      data_[i].Resize(GetSize(), new_size);
    }
    capacity_ = new_size;
  }

  for (idx_t i = 0; i < ColumnCount(); i++) {
    // The target has to be flat: there is nowhere to append to in a dictionary.
    BUMBLEBEE_ASSERT(data_[i].GetVectorType() == VectorType::FLAT_VECTOR,
                     "DataChunk::Append: the target column is not a flat vector");
    if (sel != nullptr) {
      VectorOperations::Copy(other.data_[i], data_[i], *sel, count, 0, GetSize());
    } else {
      VectorOperations::Copy(other.data_[i], data_[i], other.GetSize(), 0, GetSize());
    }
  }
  SetCardinality(new_size);
}

void DataChunk::Resize(idx_t size) {
  for (auto &v : data_) {
    v.Resize(count_, size);
  }
  capacity_ = size;
}

void DataChunk::Destroy() {
  data_.clear();
  cache_types_.clear();
  cache_mngrs_.clear();
  capacity_ = 0;
  count_ = 0;
}

void DataChunk::Copy(DataChunk &other, idx_t offset) const {
  BUMBLEBEE_ASSERT(ColumnCount() == other.ColumnCount(), "DataChunk::Copy: the column counts do not match");
  // `other` is the target, so it should be empty.
  BUMBLEBEE_ASSERT(other.GetSize() == 0, "DataChunk::Copy: the target chunk is not empty");

  for (idx_t i = 0; i < ColumnCount(); i++) {
    BUMBLEBEE_ASSERT(other.data_[i].GetVectorType() == VectorType::FLAT_VECTOR,
                     "DataChunk::Copy: the target column is not a flat vector");
    VectorOperations::Copy(data_[i], other.data_[i], GetSize(), offset, 0);
  }
  other.SetCardinality(GetSize() - offset);
}

void DataChunk::Copy(DataChunk &other, const SelectionVector &sel, idx_t source_count, idx_t offset) const {
  BUMBLEBEE_ASSERT(ColumnCount() == other.ColumnCount(), "DataChunk::Copy: the column counts do not match");

  for (idx_t i = 0; i < ColumnCount(); i++) {
    BUMBLEBEE_ASSERT(other.data_[i].GetVectorType() == VectorType::FLAT_VECTOR,
                     "DataChunk::Copy: the target column is not a flat vector");
    VectorOperations::Copy(data_[i], other.data_[i], sel, source_count, offset, 0);
  }
  other.SetCardinality(source_count - offset);
}

void DataChunk::Split(DataChunk &other, idx_t split_index) {
  BUMBLEBEE_ASSERT(other.GetSize() == 0, "DataChunk::Split: the target chunk is not empty");
  BUMBLEBEE_ASSERT(other.data_.empty(), "DataChunk::Split: the target chunk already has columns");
  BUMBLEBEE_ASSERT(split_index < data_.size(), "DataChunk::Split: the split index is out of range");

  const idx_t num_cols = data_.size();
  for (idx_t col = split_index; col < num_cols; col++) {
    other.data_.push_back(std::move(data_[col]));
  }
  for (idx_t col = split_index; col < num_cols; col++) {
    data_.pop_back();
  }
  other.SetCapacity(*this);
  other.SetCardinality(*this);
}

void DataChunk::Normalify() {
  for (idx_t i = 0; i < ColumnCount(); i++) {
    data_[i].Normalify(count_);
  }
}

auto DataChunk::Orrify() -> array_vector_data_t {
  array_vector_data_t result(new VectorData[ColumnCount()]);
  for (idx_t i = 0; i < ColumnCount(); i++) {
    data_[i].Orrify(count_, result.get()[i]);
  }
  return result;
}

void DataChunk::Slice(const SelectionVector &sel_vector, idx_t count) {
  count_ = count;
  for (idx_t i = 0; i < ColumnCount(); i++) {
    data_[i].Slice(sel_vector, count);
  }
}

void DataChunk::Slice(DataChunk &other, const SelectionVector &sel, idx_t count, idx_t col_offset) {
  BUMBLEBEE_ASSERT(ColumnCount() >= other.ColumnCount() + col_offset, "DataChunk::Slice: not enough columns");
  count_ = count;
  SelCache merge_cache;
  for (idx_t c = 0; c < other.ColumnCount(); c++) {
    auto &src = other.data_[c];
    auto &dst = data_[col_offset + c];
    if (src.GetVectorType() == VectorType::CONSTANT_VECTOR) {
      // A selection over a constant is the same constant — and when the target still references
      // that very constant from the previous batch (partial resets leave it in place), there is
      // nothing to do at all. This is what lets a pruned scan's 100 constant-NULL columns cross a
      // Filter with zero per-chunk work.
      if (dst.GetVectorType() != VectorType::CONSTANT_VECTOR || dst.DataMngrPtr() != src.DataMngrPtr()) {
        dst.Reference(src);
      }
      continue;
    }
    if (src.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
      // Already a dictionary: compose the selections instead of nesting.
      dst.Reference(src);
      dst.Slice(sel, count, merge_cache);
    } else {
      dst.Slice(src, sel, count);
    }
  }
}

void DataChunk::Slice(DataChunk &other, const SelectionVector &sel, idx_t count, const std::vector<idx_t> &cols_map) {
  BUMBLEBEE_ASSERT(cols_map.size() == other.ColumnCount(), "DataChunk::Slice: the column map does not match");
  count_ = count;
  SelCache merge_cache;
  for (idx_t c = 0; c < other.ColumnCount(); c++) {
    auto index = cols_map[c];
    BUMBLEBEE_ASSERT(index < ColumnCount(), "DataChunk::Slice: the mapped column is out of range");
    if (other.data_[c].GetVectorType() == VectorType::DICTIONARY_VECTOR) {
      data_[index].Reference(other.data_[c]);
      data_[index].Slice(sel, count, merge_cache);
    } else {
      data_[index].Slice(other.data_[c], sel, count);
    }
  }
}

void DataChunk::Reset() {
  if (data_.empty()) {
    return;
  }
  if (cache_mngrs_.size() != data_.size()) {
    // No usable cache (InitializeEmpty / Split changed the column set): rebuild from scratch
    // once; Initialize records the fresh buffers so later Resets take the cheap path.
    auto types = GetTypes();
    data_.clear();
    count_ = 0;
    Initialize(types);
    return;
  }
  for (idx_t i = 0; i < data_.size(); i++) {
    auto &vec = data_[i];
    const auto ptype = vec.GetType();
    if (ptype == PhysicalType::LIST || ptype == PhysicalType::ARRAY || ptype != cache_types_[i]) {
      // Nested children can't be reused in place, and a re-typed column outgrew its buffer:
      // re-create the vector and adopt its new buffer as the cache.
      Vector fresh(vec.GetLogicalType());
      vec.Reference(fresh);
      cache_types_[i] = ptype;
      cache_mngrs_[i] = vec.GetDataMngr();
    } else {
      vec.ResetFromCache(cache_mngrs_[i]);
    }
  }
  count_ = 0;
  capacity_ = STANDARD_VECTOR_SIZE;
}

void DataChunk::Reset(const std::vector<idx_t> &columns_to_reset) {
  if (data_.empty()) {
    return;
  }
  if (cache_mngrs_.size() != data_.size()) {
    Reset();  // no usable cache yet: the full path rebuilds it
    return;
  }
  // Only the named columns go back to a fresh flat buffer; the others are left exactly as they
  // are (a pruned scan's constant-NULL references stay valid across chunks, so re-pointing them
  // every iteration would be pure churn).
  for (const auto i : columns_to_reset) {
    BUMBLEBEE_ASSERT(i < data_.size(), "DataChunk::Reset: the column is out of range");
    auto &vec = data_[i];
    const auto ptype = vec.GetType();
    if (ptype == PhysicalType::LIST || ptype == PhysicalType::ARRAY || ptype != cache_types_[i]) {
      Vector fresh(vec.GetLogicalType());
      vec.Reference(fresh);
      cache_types_[i] = ptype;
      cache_mngrs_[i] = vec.GetDataMngr();
    } else {
      vec.ResetFromCache(cache_mngrs_[i]);
    }
  }
  count_ = 0;
  capacity_ = STANDARD_VECTOR_SIZE;
}

auto DataChunk::GetTypes() const -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(data_.size());
  for (const auto &v : data_) {
    types.push_back(v.GetLogicalType());
  }
  return types;
}

namespace {

/** The materialized bytes of `count` rows of `vec`: the inline stride plus the out-of-line payloads. */
auto VectorEstimatedBytes(Vector &vec, idx_t count) -> idx_t {
  const auto ptype = vec.GetType();
  idx_t bytes = LogicalType::SizeOf(ptype) * count;
  switch (ptype) {
    case PhysicalType::STRING: {
      // The stride holds the 24-byte handles; only the non-inlined payloads live elsewhere.
      VectorData vd;
      vec.Orrify(count, vd);
      const auto *strings = reinterpret_cast<const string_t *>(vd.data_);
      for (idx_t i = 0; i < count; i++) {
        const idx_t idx = vd.sel_->GetIndex(i);
        if (vd.validity_->RowIsValid(idx) && !strings[idx].IsInlined()) {
          bytes += strings[idx].Size();
        }
      }
      break;
    }
    case PhysicalType::LIST:
      bytes += VectorEstimatedBytes(ListVector::GetChild(vec), ListVector::GetListSize(vec));
      break;
    case PhysicalType::ARRAY:
      bytes += VectorEstimatedBytes(ArrayVector::GetChild(vec), count * ArrayVector::GetArraySize(vec));
      break;
    default:
      break;
  }
  return bytes;
}

}  // namespace

auto DataChunk::EstimatedBytes() -> idx_t {
  idx_t bytes = 0;
  for (auto &v : data_) {
    bytes += VectorEstimatedBytes(v, count_);
  }
  return bytes;
}

auto DataChunk::ToString() const -> std::string {
  std::string result = "Chunk - [" + std::to_string(ColumnCount()) + " Columns]\n";
  for (idx_t i = 0; i < ColumnCount(); i++) {
    result += "- " + data_[i].ToString(GetSize()) + "\n";
  }
  return result;
}

void DataChunk::Verify() {
  // TODO(milestone-2): a DEBUG-only structural check of the chunk.
}

void DataChunk::Hash(Vector &result) {
  BUMBLEBEE_ASSERT(result.GetType() == PhysicalType::UBIGINT, "DataChunk::Hash: the result must be a UBIGINT vector");
  if (ColumnCount() == 0) {
    return;
  }
  // The first column seeds the hash; every further column is folded into it, so that the
  // result identifies the whole row rather than any one field.
  VectorOperations::Hash(data_[0], result, GetSize());
  for (idx_t i = 1; i < ColumnCount(); i++) {
    VectorOperations::CombineHash(result, data_[i], GetSize());
  }
}

void DataChunk::Hash(Vector &result, const std::vector<idx_t> &cols) {
  BUMBLEBEE_ASSERT(result.GetType() == PhysicalType::UBIGINT, "DataChunk::Hash: the result must be a UBIGINT vector");
  if (cols.empty()) {
    return;
  }
  VectorOperations::Hash(data_[cols[0]], result, GetSize());
  for (idx_t i = 1; i < cols.size(); i++) {
    VectorOperations::CombineHash(result, data_[cols[i]], GetSize());
  }
}

void DataChunk::Cast(const std::vector<LogicalType> &types) {
  BUMBLEBEE_ASSERT(types.size() == ColumnCount(), "DataChunk::Cast: one type per column");
  for (idx_t i = 0; i < ColumnCount(); i++) {
    if (data_[i].GetLogicalType() == types[i]) {
      continue;
    }
    Vector new_vec(types[i], GetCapacity());
    VectorOperations::Cast(data_[i], new_vec, GetSize());
    // A cast is a 1:1 row mapping, so the validity carries over unchanged. Take an
    // independent copy: the source column is about to be re-pointed at `new_vec`.
    if (!data_[i].Validity().AllValid()) {
      new_vec.Validity() = data_[i].Validity().Copy();
    }
    // Reference, not move: new_vec's data manager keeps the memory alive.
    data_[i].Reference(new_vec);
  }
}

void DataChunk::Cast(DataChunk &result) {
  BUMBLEBEE_ASSERT(ColumnCount() == result.ColumnCount(), "DataChunk::Cast: the column counts do not match");
  for (idx_t i = 0; i < ColumnCount(); i++) {
    VectorOperations::Cast(data_[i], result.data_[i], GetSize());
    if (!data_[i].Validity().AllValid()) {
      result.data_[i].Validity() = data_[i].Validity().Copy();
    }
  }
  result.SetCardinality(GetSize());
}

}  // namespace bumblebee
