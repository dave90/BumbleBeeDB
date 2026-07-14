//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// list_vector.cpp
//
// Identification: src/type/vector/list_vector.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
// The nested (LIST / ARRAY) Vector accessors.
//
//===----------------------------------------------------------------------===//

#include "common/exception.h"
#include "common/helper.h"
#include "common/macros.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

// ---------------------------------------------------------------------------
// ListDataMngr
// ---------------------------------------------------------------------------

void ListDataMngr::Reserve(idx_t required) {
  if (required <= capacity_) {
    return;
  }
  // Double, so that appending row by row stays amortized O(1) per element.
  idx_t new_capacity = MaxValue<idx_t>(required, capacity_ * 2);
  child_.Resize(size_, new_capacity);
  capacity_ = new_capacity;
}

// ---------------------------------------------------------------------------
// ListVector
// ---------------------------------------------------------------------------

auto ListVector::Mngr(Vector &vector) -> ListDataMngr & {
  if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    // A sliced list: the list itself — and therefore its elements — lives on as the
    // dictionary's child.
    return Mngr(DictionaryVector::Child(vector));
  }
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::LIST, "ListVector: the vector is not a LIST");
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_ != nullptr &&
                       vector.aux_data_mngr_->GetType() == VectorDataMngrType::LIST_BUFFER,
                   "ListVector: the vector has no list buffer");
  return static_cast<ListDataMngr &>(*vector.aux_data_mngr_);
}

auto ListVector::Mngr(const Vector &vector) -> const ListDataMngr & {
  if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    return Mngr(DictionaryVector::Child(vector));
  }
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::LIST, "ListVector: the vector is not a LIST");
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_ != nullptr &&
                       vector.aux_data_mngr_->GetType() == VectorDataMngrType::LIST_BUFFER,
                   "ListVector: the vector has no list buffer");
  return static_cast<const ListDataMngr &>(*vector.aux_data_mngr_);
}

auto ListVector::GetChild(Vector &vector) -> Vector & { return Mngr(vector).GetChild(); }

auto ListVector::GetChild(const Vector &vector) -> const Vector & { return Mngr(vector).GetChild(); }

auto ListVector::GetListSize(const Vector &vector) -> idx_t { return Mngr(vector).GetSize(); }

void ListVector::SetListSize(Vector &vector, idx_t size) { Mngr(vector).SetSize(size); }

void ListVector::Reserve(Vector &vector, idx_t required) { Mngr(vector).Reserve(required); }

auto ListVector::Append(Vector &vector, const Vector &source, idx_t source_count, idx_t source_offset) -> idx_t {
  auto &mngr = Mngr(vector);
  const idx_t old_size = mngr.GetSize();
  BUMBLEBEE_ASSERT(source_offset <= source_count, "ListVector::Append: the source offset is past the source");
  const idx_t append_count = source_count - source_offset;
  if (append_count == 0) {
    return old_size;
  }
  mngr.Reserve(old_size + append_count);
  // Copy, not reference: the elements have to belong to THIS vector's child, or the
  // offsets we hand back would point into memory owned by the source.
  VectorOperations::Copy(source, mngr.GetChild(), source_count, source_offset, old_size);
  mngr.SetSize(old_size + append_count);
  return old_size;
}

auto ListVector::GetEntries(Vector &vector) -> ListEntry * {
  if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    // The entries of the list underneath: they are indexed by the dictionary's selection.
    return GetEntries(DictionaryVector::Child(vector));
  }
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::LIST, "ListVector::GetEntries: the vector is not a LIST");
  return FlatVector::GetData<ListEntry>(vector);
}

auto ListVector::GetEntries(const Vector &vector) -> const ListEntry * {
  if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    return GetEntries(DictionaryVector::Child(vector));
  }
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::LIST, "ListVector::GetEntries: the vector is not a LIST");
  return FlatVector::GetData<ListEntry>(vector);
}

// ---------------------------------------------------------------------------
// ArrayVector
// ---------------------------------------------------------------------------

auto ArrayVector::Mngr(Vector &vector) -> ArrayDataMngr & {
  if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    return Mngr(DictionaryVector::Child(vector));
  }
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::ARRAY, "ArrayVector: the vector is not an ARRAY");
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_ != nullptr &&
                       vector.aux_data_mngr_->GetType() == VectorDataMngrType::ARRAY_BUFFER,
                   "ArrayVector: the vector has no array buffer");
  return static_cast<ArrayDataMngr &>(*vector.aux_data_mngr_);
}

auto ArrayVector::Mngr(const Vector &vector) -> const ArrayDataMngr & {
  if (vector.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    return Mngr(DictionaryVector::Child(vector));
  }
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::ARRAY, "ArrayVector: the vector is not an ARRAY");
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_ != nullptr &&
                       vector.aux_data_mngr_->GetType() == VectorDataMngrType::ARRAY_BUFFER,
                   "ArrayVector: the vector has no array buffer");
  return static_cast<const ArrayDataMngr &>(*vector.aux_data_mngr_);
}

auto ArrayVector::GetChild(Vector &vector) -> Vector & { return Mngr(vector).GetChild(); }

auto ArrayVector::GetChild(const Vector &vector) -> const Vector & { return Mngr(vector).GetChild(); }

auto ArrayVector::GetArraySize(const Vector &vector) -> idx_t { return Mngr(vector).GetArraySize(); }

}  // namespace bumblebee
