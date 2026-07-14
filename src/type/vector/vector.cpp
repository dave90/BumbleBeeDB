//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector.cpp
//
// Identification: src/type/vector/vector.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/vector.h"

#include <cstring>
#include <utility>

#include "common/exception.h"
#include "common/helper.h"
#include "type/null_value.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

Vector::Vector(Vector &other) : vtype_(other.vtype_), type_(other.type_) { Reference(other); }

Vector::Vector(Vector &other, const SelectionVector &sel, idx_t count) : type_(other.type_) { Slice(other, sel, count); }

Vector::Vector(Vector &other, idx_t offset) : vtype_(other.vtype_), type_(other.type_) { Slice(other, offset); }

Vector::Vector(const Value &value) : type_(value.GetType()) {
  // UPGRADE over the original, which took the value's bare PhysicalType: a BOOLEAN or a
  // DATE constant keeps its logical identity instead of decaying to UTINYINT / INTEGER.
  Reference(value);
}

Vector::Vector(LogicalType type, idx_t capacity) : type_(std::move(type)) { Initialize(capacity); }

Vector::Vector(LogicalType type, data_ptr_t dataptr) : type_(std::move(type)), data_(dataptr) { CheckSupportedType(); }

Vector::Vector(LogicalType type, bool create_data, bool zero_data, idx_t capacity)
    : type_(std::move(type)), data_(nullptr) {
  if (create_data) {
    Initialize(zero_data, capacity);
  } else {
    CheckSupportedType();
  }
}

Vector::Vector(Vector &&other) noexcept
    : vtype_(other.vtype_),
      type_(std::move(other.type_)),
      data_(other.data_),
      data_mngr_(std::move(other.data_mngr_)),
      aux_data_mngr_(std::move(other.aux_data_mngr_)),
      validity_(std::move(other.validity_)) {}

void Vector::CheckSupportedType() const {
  switch (type_.GetPhysicalType()) {
    case PhysicalType::STRUCT:
      // TODO(milestone-2): struct vectors — one child Vector per field.
      throw NotImplementedException(
          fmt::format("Vector: {} is not supported yet", LogicalType::NameOf(type_.GetPhysicalType())));
    default:
      break;
  }
}

void Vector::InitializeAuxiliary(idx_t capacity) {
  switch (type_.GetPhysicalType()) {
    case PhysicalType::LIST: {
      // The elements start out in a child of the standard size; ListVector::Reserve grows
      // it as rows are written, since a LIST row may hold any number of elements.
      Vector child(type_.GetChildType(), true, true, STANDARD_VECTOR_SIZE);
      aux_data_mngr_ = vector_data_mngr_ptr_t(new ListDataMngr(std::move(child), STANDARD_VECTOR_SIZE));
      break;
    }
    case PhysicalType::ARRAY: {
      // An ARRAY row IS a fixed slice of the child, so the child is sized exactly.
      const idx_t array_size = type_.GetListData().size_;
      BUMBLEBEE_ASSERT(array_size > 0, "Vector: an ARRAY type must have a size");
      Vector child(type_.GetChildType(), true, true, MaxValue<idx_t>(capacity * array_size, 1));
      aux_data_mngr_ = vector_data_mngr_ptr_t(new ArrayDataMngr(std::move(child), array_size));
      break;
    }
    default:
      break;
  }
}

void Vector::Reference(const Value &value) {
  if (!value.IsNull() && value.GetPhysicalType() != type_.GetPhysicalType()) {
    // The vector has no type yet (or a mismatching one): adopt the value's.
    type_ = value.GetType();
  }
  CheckSupportedType();
  vtype_ = VectorType::CONSTANT_VECTOR;
  data_mngr_ = VectorDataMngr::CreateConstantVector(type_.GetPhysicalType());
  data_ = data_mngr_->GetData();
  // The auxiliary manager (string heap / dictionary child) of whatever we held is gone.
  aux_data_mngr_.reset();
  // A nested constant still needs somewhere to put its elements: one row's worth.
  InitializeAuxiliary(1);
  // Fresh all-valid mask; SetValue clears the bit when the value is NULL.
  validity_.Reset();
  // The single constant lives at position 0.
  SetValue(0, value);
}

void Vector::Reference(const Vector &other) { Reinterpret(other); }

void Vector::Reinterpret(const Vector &other) {
  type_ = other.type_;
  vtype_ = other.vtype_;
  data_ = other.data_;
  AssignSharedPointer(data_mngr_, other.data_mngr_);
  AssignSharedPointer(aux_data_mngr_, other.aux_data_mngr_);
  // Share the validity buffer (shallow): referenced vectors share their nulls.
  validity_ = other.validity_;
}

void Vector::ReferenceAndSetType(Vector &other) {
  type_ = other.type_;
  Reference(other);
}

void Vector::Slice(Vector &other, idx_t offset) {
  if (other.vtype_ == VectorType::CONSTANT_VECTOR) {
    // A constant is the same value at every row: the offset changes nothing.
    Reference(other);
    return;
  }
  BUMBLEBEE_ASSERT(other.GetVectorType() == VectorType::FLAT_VECTOR,
                   "Vector::Slice(offset) only works on flat vectors; slice a dictionary with a selection");
  Reference(other);
  if (offset > 0) {
    if (type_.GetPhysicalType() == PhysicalType::ARRAY) {
      // An ARRAY has no inline payload to move: the rows live in the child, `array_size`
      // slots each, so it is the CHILD that has to be shifted.
      const idx_t array_size = ArrayVector::GetArraySize(other);
      Vector child(ArrayVector::GetChild(other), offset * array_size);
      aux_data_mngr_ = vector_data_mngr_ptr_t(new ArrayDataMngr(std::move(child), array_size));
      if (!other.validity_.AllValid()) {
        validity_.Slice(other.validity_, offset);
      }
      return;
    }
    // Move the data by the offset. A LIST moves its entries; the child is shared, and the
    // offsets the entries hold are absolute, so they keep pointing at the right elements.
    data_ = data_ + LogicalType::SizeOf(type_.GetPhysicalType()) * offset;
    // The mask has to shift with the data: produce an independent, shifted copy — but
    // only when nulls are actually present, so that all-valid stays zero-copy.
    if (!other.validity_.AllValid()) {
      validity_.Slice(other.validity_, offset);
    }
  }
}

void Vector::Slice(Vector &other, const SelectionVector &sel, idx_t count) {
  Reference(other);
  Slice(sel, count);
}

void Vector::Slice(const SelectionVector &sel, idx_t count) {
  if (vtype_ == VectorType::CONSTANT_VECTOR) {
    // A selection over a constant is the same constant.
    return;
  }
  if (vtype_ == VectorType::DICTIONARY_VECTOR) {
    // Compose the two selections instead of nesting the dictionaries.
    auto current_sel = DictionaryVector::SelVector(*this);
    auto new_sel = current_sel.Slice(sel, count);
    data_mngr_ = vector_data_mngr_ptr_t(new DictionaryDataMngr(std::move(new_sel)));
    return;
  }
  // Any other encoding becomes the child of a fresh dictionary.
  Vector child(*this);
  vtype_ = VectorType::DICTIONARY_VECTOR;
  data_mngr_ = vector_data_mngr_ptr_t(new DictionaryDataMngr(sel));
  aux_data_mngr_ = vector_data_mngr_ptr_t(new VectorChildDataMngr(std::move(child)));
  // The authoritative mask now lives on the child, read through the selection.
  validity_.Reset();
}

void Vector::Slice(const SelectionVector &sel, idx_t count, SelCache &cache) {
  if (GetVectorType() != VectorType::DICTIONARY_VECTOR) {
    Slice(sel, count);
    return;
  }
  // Slicing several dictionary vectors by one selection composes the same two selections
  // over and over: do it once and share the result.
  auto &current_sel = DictionaryVector::SelVector(*this);
  auto *target_data = current_sel.GetData();
  auto entry = cache.cache_.find(target_data);
  if (entry != cache.cache_.end()) {
    auto sel_cached = static_cast<DictionaryDataMngr &>(*entry->second).GetSelection();
    data_mngr_ = vector_data_mngr_ptr_t(new DictionaryDataMngr(sel_cached));
    vtype_ = VectorType::DICTIONARY_VECTOR;
    return;
  }
  Slice(sel, count);
  cache.cache_[target_data] = data_mngr_;
}

void Vector::Swap(Vector &other) {
  Vector tmp(other);
  other.Reference(*this);
  Reference(tmp);
}

void Vector::Initialize(bool zero_data, idx_t capacity) {
  CheckSupportedType();
  aux_data_mngr_.reset();
  data_mngr_ = VectorDataMngr::CreateStandardVector(type_.GetPhysicalType(), capacity);
  data_ = data_mngr_->GetData();
  // A LIST / ARRAY needs a child Vector to put its elements in. (An ARRAY's own
  // allocation above is empty: SizeOf(ARRAY) is 0, because an ARRAY row has no inline
  // payload at all.)
  InitializeAuxiliary(capacity);
  // A fresh vector is all-valid.
  validity_.Reset();
  if (zero_data || type_.GetPhysicalType() == PhysicalType::LIST) {
    // The entries of a LIST are always zeroed: a garbage (offset, length) would send any
    // reader off into the child at random, so an unwritten row must read as an empty list.
    memset(data_, 0, LogicalType::SizeOf(type_.GetPhysicalType()) * capacity);
  }
}

auto Vector::ToString(idx_t count) const -> std::string {
  std::string s = "Vector type(" + std::to_string(static_cast<int>(vtype_)) + ", " + type_.ToString() + "[";
  if (data_ == nullptr && vtype_ != VectorType::SEQUENCE_VECTOR && vtype_ != VectorType::SEQUENCE_CIRCULAR_VECTOR) {
    return s + " NULL ]";
  }
  switch (vtype_) {
    case VectorType::CONSTANT_VECTOR:
      s += GetValue(0).ToString();
      break;
    case VectorType::FLAT_VECTOR:
    case VectorType::DICTIONARY_VECTOR:
    case VectorType::SEQUENCE_VECTOR:
    case VectorType::SEQUENCE_CIRCULAR_VECTOR:
      for (idx_t i = 0; i < count; i++) {
        s += GetValue(i).ToString() + ", ";
      }
      break;
  }
  return s + "]";
}

auto Vector::ToString() const -> std::string {
  std::string s = "Vector type(" + std::to_string(static_cast<int>(vtype_)) + "), " + type_.ToString() + " [";
  if (vtype_ == VectorType::CONSTANT_VECTOR) {
    s += GetValue(0).ToString();
  }
  return s + "]";
}

/** @brief Replicate the constant stored at position 0 of `old_data` over `count` rows. */
template <class T>
static void TemplatedFlattenConstantVector(data_ptr_t data, data_ptr_t old_data, idx_t count) {
  auto constant = Load<T>(old_data);
  auto *output = reinterpret_cast<T *>(data);
  for (idx_t i = 0; i < count; i++) {
    output[i] = constant;
  }
}

void Vector::Normalify(idx_t count) {
  switch (vtype_) {
    case VectorType::FLAT_VECTOR:
      return;
    case VectorType::DICTIONARY_VECTOR: {
      idx_t new_size = MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE);
      Vector norm_vec(type_, new_size);
      VectorOperations::Copy(*this, norm_vec, count, 0, 0);
      Reference(norm_vec);
      return;
    }
    case VectorType::SEQUENCE_VECTOR: {
      int64_t start;
      int64_t increment;
      SequenceVector::GetSequence(*this, start, increment);
      data_mngr_ = VectorDataMngr::CreateStandardVector(GetType());
      data_ = data_mngr_->GetData();
      VectorOperations::GenerateSequence(*this, count, start, increment);
      return;
    }
    case VectorType::SEQUENCE_CIRCULAR_VECTOR: {
      int64_t start;
      int64_t offset;
      int64_t stride;
      int64_t end;
      CircularSequenceVector::GetSequence(*this, start, offset, stride, end);
      data_mngr_ = VectorDataMngr::CreateStandardVector(GetType());
      data_ = data_mngr_->GetData();
      VectorOperations::GenerateSequence(*this, count, start, offset, stride, end);
      return;
    }
    case VectorType::CONSTANT_VECTOR:
      break;
  }

  if (type_.GetPhysicalType() == PhysicalType::LIST || type_.GetPhysicalType() == PhysicalType::ARRAY) {
    // A nested constant cannot be flattened by replicating its inline payload: copying the
    // single ListEntry into every row would leave `count` rows ALIASING one range of the
    // child, so writing into row 0's elements would silently change row 1's. Read the
    // constant out as a Value and write it back row by row instead — each row then gets its
    // own, independent range of elements.
    const Value constant = GetValue(0);
    Initialize(false, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
    vtype_ = VectorType::FLAT_VECTOR;
    for (idx_t i = 0; i < count; i++) {
      SetValue(i, constant);
    }
    return;
  }

  // Hold on to the old manager: dropping it would free the constant we are about to read.
  auto old_data_mngr = data_mngr_;
  auto *old_data = old_data_mngr->GetData();
  // A constant carries a single validity bit (row 0): replicate it over every row.
  bool constant_is_null = !validity_.RowIsValid(0);
  validity_.Reset();
  data_mngr_ = VectorDataMngr::CreateStandardVector(GetType());
  data_ = data_mngr_->GetData();
  vtype_ = VectorType::FLAT_VECTOR;
  switch (type_.GetPhysicalType()) {
    case PhysicalType::TINYINT:
      TemplatedFlattenConstantVector<int8_t>(data_, old_data, count);
      break;
    case PhysicalType::SMALLINT:
      TemplatedFlattenConstantVector<int16_t>(data_, old_data, count);
      break;
    case PhysicalType::INTEGER:
      TemplatedFlattenConstantVector<int32_t>(data_, old_data, count);
      break;
    case PhysicalType::BIGINT:
      TemplatedFlattenConstantVector<int64_t>(data_, old_data, count);
      break;
    case PhysicalType::UTINYINT:
      TemplatedFlattenConstantVector<uint8_t>(data_, old_data, count);
      break;
    case PhysicalType::USMALLINT:
      TemplatedFlattenConstantVector<uint16_t>(data_, old_data, count);
      break;
    case PhysicalType::UINTEGER:
      TemplatedFlattenConstantVector<uint32_t>(data_, old_data, count);
      break;
    case PhysicalType::UBIGINT:
      TemplatedFlattenConstantVector<uint64_t>(data_, old_data, count);
      break;
    case PhysicalType::FLOAT:
      TemplatedFlattenConstantVector<float>(data_, old_data, count);
      break;
    case PhysicalType::DOUBLE:
      TemplatedFlattenConstantVector<double>(data_, old_data, count);
      break;
    case PhysicalType::STRING:
      TemplatedFlattenConstantVector<string_t>(data_, old_data, count);
      break;
    default:
      // TODO(milestone-2 step 8): list vectors
      throw NotImplementedException(
          fmt::format("Vector::Normalify: unsupported type {}", LogicalType::NameOf(type_.GetPhysicalType())));
  }
  if (constant_is_null) {
    validity_.SetAllInvalid(count);
  }
}

void Vector::Normalify(const SelectionVector &sel, idx_t count) {
  switch (vtype_) {
    case VectorType::FLAT_VECTOR:
      return;
    case VectorType::SEQUENCE_VECTOR: {
      int64_t start;
      int64_t increment;
      SequenceVector::GetSequence(*this, start, increment);
      data_mngr_ = VectorDataMngr::CreateStandardVector(GetType());
      data_ = data_mngr_->GetData();
      VectorOperations::GenerateSequence(*this, count, sel, start, increment);
      return;
    }
    case VectorType::SEQUENCE_CIRCULAR_VECTOR: {
      int64_t start;
      int64_t offset;
      int64_t stride;
      int64_t end;
      CircularSequenceVector::GetSequence(*this, start, offset, stride, end);
      data_mngr_ = VectorDataMngr::CreateStandardVector(GetType());
      data_ = data_mngr_->GetData();
      VectorOperations::GenerateSequence(*this, count, sel, start, offset, stride, end);
      return;
    }
    default:
      throw NotImplementedException("Vector::Normalify(sel): unsupported vector type");
  }
}

void Vector::Orrify(idx_t count, VectorData &data) {
  switch (vtype_) {
    case VectorType::DICTIONARY_VECTOR: {
      auto &sel_vector = DictionaryVector::SelVector(*this);
      auto &child = DictionaryVector::Child(*this);
      if (child.GetVectorType() == VectorType::FLAT_VECTOR) {
        data.data_ = child.data_;
        data.sel_ = &sel_vector;
        // The validity lives on the child, indexed — like the data — through the selection.
        data.validity_ = &child.validity_;
        return;
      }
      // The child is not flat: materialize it (only at the selected rows) and keep it.
      Vector child_flat(child);
      child_flat.Normalify(sel_vector, count);
      aux_data_mngr_ = vector_data_mngr_ptr_t(new VectorChildDataMngr(std::move(child_flat)));
      data.data_ = FlatVector::GetData(DictionaryVector::Child(*this));
      data.sel_ = &sel_vector;
      data.validity_ = &DictionaryVector::Child(*this).validity_;
      return;
    }
    case VectorType::CONSTANT_VECTOR:
      data.sel_ = ConstantVector::ZeroSelectionVector(count, data.owned_sel_);
      data.data_ = ConstantVector::GetData(*this);
      // The single validity bit is at row 0, and the zero selection makes every read hit it.
      data.validity_ = &validity_;
      return;
    default:
      break;
  }
  Normalify(count);
  data.sel_ = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
  data.data_ = FlatVector::GetData(*this);
  data.validity_ = &validity_;
}

void Vector::Sequence(int64_t start, int64_t increment) {
  vtype_ = VectorType::SEQUENCE_VECTOR;
  aux_data_mngr_.reset();
  // The data manager now holds the sequence parameters, not rows: whatever data_ pointed
  // at is about to be freed, so drop it. Normalify() re-points it at the materialized rows.
  data_ = nullptr;
  data_mngr_ = vector_data_mngr_ptr_t(new VectorDataMngr(2 * sizeof(int64_t)));
  auto *data = reinterpret_cast<int64_t *>(data_mngr_->GetData());
  data[0] = start;
  data[1] = increment;
}

void Vector::Sequence(int64_t start, int64_t offset, int64_t stride, int64_t end) {
  BUMBLEBEE_ASSERT(start < end, "Vector::Sequence: the circular sequence needs start < end");
  vtype_ = VectorType::SEQUENCE_CIRCULAR_VECTOR;
  aux_data_mngr_.reset();
  data_ = nullptr;
  data_mngr_ = vector_data_mngr_ptr_t(new VectorDataMngr(4 * sizeof(int64_t)));
  auto *data = reinterpret_cast<int64_t *>(data_mngr_->GetData());
  data[0] = start;
  data[1] = offset;
  data[2] = stride;
  data[3] = end;
}

void Vector::Verify(idx_t count) {
  // TODO(milestone-2): a DEBUG-only structural check of the vector.
  (void)count;
}

void Vector::Verify(const SelectionVector &sel, idx_t count) {
  // TODO(milestone-2): a DEBUG-only structural check of the selected rows.
  (void)sel;
  (void)count;
}

auto Vector::WrapValue(Value raw) const -> Value {
  if (raw.GetType() == type_) {
    return raw;
  }
  switch (type_.GetTypeId()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::STRING:
      return raw.CastAs(type_);
    default:
      // DATE / TIMESTAMP / DECIMAL / HASH / ADDRESS: Value has no constructor that
      // produces one, so the row keeps the logical type of its physical representation.
      return raw;
  }
}

auto Vector::GetValue(idx_t index) const -> Value {
  switch (GetVectorType()) {
    case VectorType::CONSTANT_VECTOR:
      // A constant holds its single value at row 0.
      index = 0;
      break;
    case VectorType::DICTIONARY_VECTOR: {
      index = DictionaryVector::SelVector(*this).GetIndex(index);
      return DictionaryVector::Child(*this).GetValue(index);
    }
    case VectorType::SEQUENCE_VECTOR: {
      int64_t start;
      int64_t increment;
      SequenceVector::GetSequence(*this, start, increment);
      return WrapValue(Value(start + static_cast<int64_t>(index) * increment));
    }
    case VectorType::SEQUENCE_CIRCULAR_VECTOR: {
      int64_t start;
      int64_t offset;
      int64_t stride;
      int64_t end;
      CircularSequenceVector::GetSequence(*this, start, offset, stride, end);
      int64_t size = end - start + 1;
      int64_t val = start + (static_cast<int64_t>(index) + offset) / stride % size;
      return WrapValue(Value(val));
    }
    case VectorType::FLAT_VECTOR:
      break;
  }
  // FLAT or CONSTANT: the index is resolved (0 for a constant).
  if (!validity_.RowIsValid(index)) {
    return Value::Null(type_);
  }
  switch (type_.GetPhysicalType()) {
    case PhysicalType::TINYINT:
      return WrapValue(Value(reinterpret_cast<int8_t *>(data_)[index]));
    case PhysicalType::SMALLINT:
      return WrapValue(Value(reinterpret_cast<int16_t *>(data_)[index]));
    case PhysicalType::INTEGER:
      return WrapValue(Value(reinterpret_cast<int32_t *>(data_)[index]));
    case PhysicalType::BIGINT:
      return WrapValue(Value(reinterpret_cast<int64_t *>(data_)[index]));
    case PhysicalType::UTINYINT:
      return WrapValue(Value(reinterpret_cast<uint8_t *>(data_)[index]));
    case PhysicalType::USMALLINT:
      return WrapValue(Value(reinterpret_cast<uint16_t *>(data_)[index]));
    case PhysicalType::UINTEGER:
      return WrapValue(Value(reinterpret_cast<uint32_t *>(data_)[index]));
    case PhysicalType::UBIGINT:
      return WrapValue(Value(reinterpret_cast<uint64_t *>(data_)[index]));
    case PhysicalType::FLOAT:
      return WrapValue(Value(reinterpret_cast<float *>(data_)[index]));
    case PhysicalType::DOUBLE:
      return WrapValue(Value(reinterpret_cast<double *>(data_)[index]));
    case PhysicalType::STRING:
      return WrapValue(Value(reinterpret_cast<string_t *>(data_)[index].GetString()));
    case PhysicalType::LIST: {
      // The row's elements are the child slice [offset, offset + length).
      const auto &entry = ListVector::GetEntries(*this)[index];
      const auto &child = ListVector::GetChild(*this);
      std::vector<Value> children;
      children.reserve(entry.length_);
      for (idx_t i = 0; i < entry.length_; i++) {
        children.push_back(child.GetValue(entry.offset_ + i));
      }
      return Value::List(type_, std::move(children));
    }
    case PhysicalType::ARRAY: {
      // Row i is the child slice [i * array_size, (i + 1) * array_size): no entry needed.
      const idx_t array_size = ArrayVector::GetArraySize(*this);
      const auto &child = ArrayVector::GetChild(*this);
      std::vector<Value> children;
      children.reserve(array_size);
      for (idx_t i = 0; i < array_size; i++) {
        children.push_back(child.GetValue(index * array_size + i));
      }
      return Value::List(type_, std::move(children));
    }
    default:
      throw NotImplementedException(
          fmt::format("Vector::GetValue: unsupported type {}", LogicalType::NameOf(type_.GetPhysicalType())));
  }
}

void Vector::SetValue(idx_t index, const Value &val) {
  if (GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    // Resolve the real row in the child and write there.
    index = DictionaryVector::SelVector(*this).GetIndex(index);
    DictionaryVector::Child(*this).SetValue(index, val);
    return;
  }
  if (val.IsNull()) {
    // Clear the validity bit, and write a defensive NULL fill so that a raw-data reader
    // never observes garbage. The fill is never used to *detect* nullness.
    WriteNullFill(index);
    validity_.SetInvalid(index);
    return;
  }

  BUMBLEBEE_ASSERT(val.GetPhysicalType() == type_.GetPhysicalType(),
                   "Vector::SetValue: the value has a different physical type than the vector");

  if (GetVectorType() == VectorType::SEQUENCE_VECTOR || GetVectorType() == VectorType::SEQUENCE_CIRCULAR_VECTOR) {
    throw NotImplementedException("Vector::SetValue: a sequence vector is generated, not written");
  }
  // A previously-null slot becomes valid again (a no-op when the mask is all-valid).
  validity_.SetValid(index);

  // FLAT or CONSTANT vector.
  switch (type_.GetPhysicalType()) {
    case PhysicalType::TINYINT:
      reinterpret_cast<int8_t *>(data_)[index] = val.GetAs<int8_t>();
      break;
    case PhysicalType::SMALLINT:
      reinterpret_cast<int16_t *>(data_)[index] = val.GetAs<int16_t>();
      break;
    case PhysicalType::INTEGER:
      reinterpret_cast<int32_t *>(data_)[index] = val.GetAs<int32_t>();
      break;
    case PhysicalType::BIGINT:
      reinterpret_cast<int64_t *>(data_)[index] = val.GetAs<int64_t>();
      break;
    case PhysicalType::UTINYINT:
      reinterpret_cast<uint8_t *>(data_)[index] = val.GetAs<uint8_t>();
      break;
    case PhysicalType::USMALLINT:
      reinterpret_cast<uint16_t *>(data_)[index] = val.GetAs<uint16_t>();
      break;
    case PhysicalType::UINTEGER:
      reinterpret_cast<uint32_t *>(data_)[index] = val.GetAs<uint32_t>();
      break;
    case PhysicalType::UBIGINT:
      reinterpret_cast<uint64_t *>(data_)[index] = val.GetAs<uint64_t>();
      break;
    case PhysicalType::FLOAT:
      reinterpret_cast<float *>(data_)[index] = val.GetAs<float>();
      break;
    case PhysicalType::DOUBLE:
      reinterpret_cast<double *>(data_)[index] = val.GetAs<double>();
      break;
    case PhysicalType::STRING:
      reinterpret_cast<string_t *>(data_)[index] = StringVector::AddString(*this, val.GetString());
      break;
    case PhysicalType::LIST: {
      // Append the elements at the end of the child and point the row's entry at them.
      // Appending (rather than overwriting the range the row used to hold) is what keeps
      // the other rows intact: nothing else ever points at the range we just took.
      const auto &children = val.GetChildren();
      const idx_t offset = ListVector::GetListSize(*this);
      ListVector::Reserve(*this, offset + children.size());
      auto &child = ListVector::GetChild(*this);
      for (idx_t i = 0; i < children.size(); i++) {
        child.SetValue(offset + i, children[i]);
      }
      ListVector::SetListSize(*this, offset + children.size());
      ListVector::GetEntries(*this)[index] = ListEntry{offset, children.size()};
      break;
    }
    case PhysicalType::ARRAY: {
      const idx_t array_size = ArrayVector::GetArraySize(*this);
      const auto &children = val.GetChildren();
      if (children.size() != array_size) {
        throw Exception(ExceptionType::MISMATCH_TYPE,
                        fmt::format("Vector::SetValue: an {} row takes exactly {} elements, not {}",
                                    type_.ToString(), array_size, children.size()));
      }
      auto &child = ArrayVector::GetChild(*this);
      for (idx_t i = 0; i < array_size; i++) {
        child.SetValue(index * array_size + i, children[i]);
      }
      break;
    }
    default:
      throw NotImplementedException(
          fmt::format("Vector::SetValue: unsupported type {}", LogicalType::NameOf(type_.GetPhysicalType())));
  }
}

void Vector::WriteNullFill(idx_t index) {
  // A defensive physical fill for a NULL slot. Never used to detect nullness.
  switch (type_.GetPhysicalType()) {
    case PhysicalType::TINYINT:
      reinterpret_cast<int8_t *>(data_)[index] = NullValue<int8_t>();
      break;
    case PhysicalType::SMALLINT:
      reinterpret_cast<int16_t *>(data_)[index] = NullValue<int16_t>();
      break;
    case PhysicalType::INTEGER:
      reinterpret_cast<int32_t *>(data_)[index] = NullValue<int32_t>();
      break;
    case PhysicalType::BIGINT:
      reinterpret_cast<int64_t *>(data_)[index] = NullValue<int64_t>();
      break;
    case PhysicalType::UTINYINT:
      reinterpret_cast<uint8_t *>(data_)[index] = NullValue<uint8_t>();
      break;
    case PhysicalType::USMALLINT:
      reinterpret_cast<uint16_t *>(data_)[index] = NullValue<uint16_t>();
      break;
    case PhysicalType::UINTEGER:
      reinterpret_cast<uint32_t *>(data_)[index] = NullValue<uint32_t>();
      break;
    case PhysicalType::UBIGINT:
      reinterpret_cast<uint64_t *>(data_)[index] = NullValue<uint64_t>();
      break;
    case PhysicalType::FLOAT:
      reinterpret_cast<float *>(data_)[index] = NullValue<float>();
      break;
    case PhysicalType::DOUBLE:
      reinterpret_cast<double *>(data_)[index] = NullValue<double>();
      break;
    case PhysicalType::STRING:
      reinterpret_cast<string_t *>(data_)[index] = NullValue<string_t>();
      break;
    case PhysicalType::LIST:
      // A NULL list reads as an EMPTY range of the child. That is only the physical fill —
      // the validity bit, which SetValue clears right after, is what makes the row NULL.
      // An empty list (length 0) that is NOT null is a different row, and the two are told
      // apart by that bit, never by this fill.
      ListVector::GetEntries(*this)[index] = ListEntry{0, 0};
      break;
    case PhysicalType::ARRAY: {
      // An ARRAY row has no inline payload to fill, but its child slice would keep whatever
      // it held: NULL the elements, so that nothing ever reads a stale one back.
      const idx_t array_size = ArrayVector::GetArraySize(*this);
      auto &child = ArrayVector::GetChild(*this);
      const auto null_element = Value::Null(type_.GetChildType());
      for (idx_t i = 0; i < array_size; i++) {
        child.SetValue(index * array_size + i, null_element);
      }
      break;
    }
    default:
      throw NotImplementedException(
          fmt::format("Vector::SetValue: unsupported type {} for the null fill",
                      LogicalType::NameOf(type_.GetPhysicalType())));
  }
}

auto Vector::Validity() -> ValidityMask & {
  if (vtype_ == VectorType::DICTIONARY_VECTOR) {
    return DictionaryVector::Child(*this).Validity();
  }
  return validity_;
}

auto Vector::Validity() const -> const ValidityMask & {
  if (vtype_ == VectorType::DICTIONARY_VECTOR) {
    return DictionaryVector::Child(*this).Validity();
  }
  return validity_;
}

auto Vector::RowIsValid(idx_t idx) const -> bool {
  switch (vtype_) {
    case VectorType::CONSTANT_VECTOR:
      return validity_.RowIsValid(0);
    case VectorType::DICTIONARY_VECTOR: {
      auto real_idx = DictionaryVector::SelVector(*this).GetIndex(idx);
      return DictionaryVector::Child(*this).RowIsValid(real_idx);
    }
    case VectorType::SEQUENCE_VECTOR:
    case VectorType::SEQUENCE_CIRCULAR_VECTOR:
      return true;
    case VectorType::FLAT_VECTOR:
      break;
  }
  return validity_.RowIsValid(idx);
}

void Vector::SetValid(idx_t idx) {
  switch (vtype_) {
    case VectorType::CONSTANT_VECTOR:
      validity_.SetValid(0);
      return;
    case VectorType::DICTIONARY_VECTOR: {
      auto real_idx = DictionaryVector::SelVector(*this).GetIndex(idx);
      DictionaryVector::Child(*this).SetValid(real_idx);
      return;
    }
    default:
      break;
  }
  validity_.SetValid(idx);
}

void Vector::SetInvalid(idx_t idx) {
  switch (vtype_) {
    case VectorType::CONSTANT_VECTOR:
      validity_.SetInvalid(0);
      return;
    case VectorType::DICTIONARY_VECTOR: {
      auto real_idx = DictionaryVector::SelVector(*this).GetIndex(idx);
      DictionaryVector::Child(*this).SetInvalid(real_idx);
      return;
    }
    default:
      break;
  }
  validity_.SetInvalid(idx);
}

void Vector::Resize(idx_t cur_size, idx_t new_size) {
  if (type_.GetPhysicalType() == PhysicalType::ARRAY) {
    // No inline payload to grow: the rows of an ARRAY live in the child, `array_size`
    // slots each, so growing the vector means growing the child.
    const idx_t array_size = ArrayVector::GetArraySize(*this);
    ArrayVector::GetChild(*this).Resize(cur_size * array_size, new_size * array_size);
    if (!validity_.AllValid()) {
      validity_.EnsureWritable(new_size);
    }
    return;
  }
  if (!data_mngr_) {
    data_mngr_ = vector_data_mngr_ptr_t(new VectorDataMngr(idx_t(0)));
  }
  BUMBLEBEE_ASSERT(data_ != nullptr, "Vector::Resize: the vector has no data");
  auto type_size = LogicalType::SizeOf(type_.GetPhysicalType());
  // Allocate a bigger array and move the old rows into it.
  auto new_data = std::unique_ptr<data_t[]>(new data_t[new_size * type_size]);
  memcpy(new_data.get(), data_, cur_size * type_size);
  if (type_.GetPhysicalType() == PhysicalType::LIST && new_size > cur_size) {
    // The new entries must read as empty lists, not as garbage (offset, length) pairs.
    memset(new_data.get() + cur_size * type_size, 0, (new_size - cur_size) * type_size);
  }
  data_mngr_->SetData(std::move(new_data));
  data_ = data_mngr_->GetData();
  // Grow the validity buffer to match: the existing bits are preserved, the new rows are valid.
  if (!validity_.AllValid()) {
    validity_.EnsureWritable(new_size);
  }
}

void Vector::SetVectorType(VectorType vector_type) { vtype_ = vector_type; }

// ---------------------------------------------------------------------------
// The accessor structs.
// ---------------------------------------------------------------------------

auto ConstantVector::ZeroSelectionVector(idx_t count, SelectionVector &owned_sel) -> const SelectionVector * {
  if (count <= STANDARD_VECTOR_SIZE) {
    return &ConstantVector::ZERO_SELECTION_VECTOR;
  }
  owned_sel.Initialize(count);
  for (idx_t i = 0; i < count; i++) {
    owned_sel.SetIndex(i, 0);
  }
  return &owned_sel;
}

void ConstantVector::Reference(Vector &vector, Vector &source, idx_t position, idx_t count) {
  BUMBLEBEE_ASSERT(position < count, "ConstantVector::Reference: the position is out of range");
  vector.type_ = source.type_;
  vector.Reference(source.GetValue(position));
}

auto StringVector::AddString(Vector &vector, const char *data, idx_t len) -> string_t {
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::STRING, "StringVector::AddString on a non string vector");
  if (string_t::IsInlined(len)) {
    // The bytes fit in the handle itself: no heap needed.
    return {data, static_cast<uint32_t>(len)};
  }
  if (!vector.aux_data_mngr_) {
    vector.aux_data_mngr_ = vector_data_mngr_ptr_t(new StringDataMngr());
  }
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_->GetType() == VectorDataMngrType::STRING_BUFFER,
                   "StringVector::AddString: the auxiliary manager is not a string heap");
  auto *string_data_mngr = static_cast<StringDataMngr *>(vector.aux_data_mngr_.get());
  return string_data_mngr->AddString(data, len);
}

auto StringVector::AddString(Vector &vector, const char *data) -> string_t {
  return AddString(vector, string_t(data, strlen(data)));
}

auto StringVector::AddString(Vector &vector, string_t data) -> string_t {
  return AddString(vector, data.CStr(), data.Size());
}

auto StringVector::AddString(Vector &vector, const std::string &data) -> string_t {
  return AddString(vector, data.c_str(), data.length());
}

auto StringVector::EmptyString(Vector &vector, idx_t len) -> string_t {
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::STRING, "StringVector::EmptyString on a non string vector");
  if (string_t::IsInlined(len)) {
    // The bytes fit in the handle itself: no heap needed.
    return string_t(static_cast<uint32_t>(len));
  }
  if (!vector.aux_data_mngr_) {
    vector.aux_data_mngr_ = vector_data_mngr_ptr_t(new StringDataMngr());
  }
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_->GetType() == VectorDataMngrType::STRING_BUFFER,
                   "StringVector::EmptyString: the auxiliary manager is not a string heap");
  auto *string_data_mngr = static_cast<StringDataMngr *>(vector.aux_data_mngr_.get());
  return string_data_mngr->AddEmptyString(len);
}

void StringVector::AddBuffer(Vector &vector, vector_data_mngr_ptr_t buffer) {
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::STRING, "StringVector::AddBuffer on a non string vector");
  if (!vector.aux_data_mngr_) {
    vector.aux_data_mngr_ = vector_data_mngr_ptr_t(new StringDataMngr());
  }
  BUMBLEBEE_ASSERT(vector.aux_data_mngr_->GetType() == VectorDataMngrType::STRING_BUFFER,
                   "StringVector::AddBuffer: the auxiliary manager is not a string heap");
  auto *string_data_mngr = static_cast<StringDataMngr *>(vector.aux_data_mngr_.get());
  string_data_mngr->AddHeapReference(std::move(buffer));
}

void StringVector::AddHeapReference(Vector &vector, Vector &other) {
  BUMBLEBEE_ASSERT(vector.GetType() == PhysicalType::STRING, "StringVector::AddHeapReference on a non string vector");
  if (other.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
    AddHeapReference(vector, DictionaryVector::Child(other));
    return;
  }
  if (!other.aux_data_mngr_) {
    // No heap to hold on to.
    return;
  }
  BUMBLEBEE_ASSERT(other.aux_data_mngr_->GetType() == VectorDataMngrType::STRING_BUFFER,
                   "StringVector::AddHeapReference: the source has no string heap");
  AddBuffer(vector, other.aux_data_mngr_);
}

void SequenceVector::GetSequence(const Vector &vector, int64_t &start, int64_t &increment) {
  BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::SEQUENCE_VECTOR,
                   "SequenceVector::GetSequence on a non sequence vector");
  auto *data = reinterpret_cast<const int64_t *>(vector.data_mngr_->GetData());
  start = data[0];
  increment = data[1];
}

void CircularSequenceVector::GetSequence(const Vector &vector, int64_t &start, int64_t &offset, int64_t &stride,
                                         int64_t &end) {
  BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::SEQUENCE_CIRCULAR_VECTOR,
                   "CircularSequenceVector::GetSequence on a non circular sequence vector");
  auto *data = reinterpret_cast<const int64_t *>(vector.data_mngr_->GetData());
  start = data[0];
  offset = data[1];
  stride = data[2];
  end = data[3];
}

}  // namespace bumblebee
