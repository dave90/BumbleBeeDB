//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_copy.cpp
//
// Identification: src/type/vector/operations/vector_copy.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/exception.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

template <class T, bool HAS_TARGET_SEL>
static void TemplatedCopy(const Vector &source, const SelectionVector &sel, Vector &target,
                          const SelectionVector *target_sel, idx_t source_offset, idx_t target_offset,
                          idx_t copy_count) {
  const auto *ldata = FlatVector::GetData<T>(source);
  auto *tdata = FlatVector::GetData<T>(target);
  for (idx_t i = 0; i < copy_count; i++) {
    auto source_idx = sel.GetIndex(source_offset + i);
    if constexpr (HAS_TARGET_SEL) {
      auto target_idx = target_sel->GetIndex(target_offset + i);
      tdata[target_idx] = ldata[source_idx];
    } else {
      tdata[target_offset + i] = ldata[source_idx];
    }
  }
}

template <class T>
static void TemplatedCopyTargetSelSwitch(const Vector &source, const SelectionVector &sel, Vector &target,
                                         const SelectionVector *target_sel, idx_t source_offset, idx_t target_offset,
                                         idx_t copy_count) {
  if (target_sel != nullptr) {
    TemplatedCopy<T, true>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
  } else {
    TemplatedCopy<T, false>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
  }
}

void VectorOperations::Copy(const Vector &source, Vector &target, idx_t source_count, idx_t source_offset,
                            idx_t target_offset) {
  switch (source.GetVectorType()) {
    case VectorType::DICTIONARY_VECTOR: {
      // Carry on into the child, reading it through the dictionary's selection.
      auto &child = DictionaryVector::Child(source);
      auto &dict_sel = DictionaryVector::SelVector(source);
      VectorOperations::Copy(child, target, dict_sel, source_count, source_offset, target_offset);
      break;
    }
    case VectorType::CONSTANT_VECTOR: {
      SelectionVector owned_sel;
      const auto *sel = ConstantVector::ZeroSelectionVector(source_count, owned_sel);
      VectorOperations::Copy(source, target, *sel, source_count, source_offset, target_offset);
      break;
    }
    case VectorType::FLAT_VECTOR:
      VectorOperations::Copy(source, target, FlatVector::INCREMENTAL_SELECTION_VECTOR, nullptr, source_count,
                             source_offset, target_offset);
      break;
    case VectorType::SEQUENCE_VECTOR: {
      int64_t start;
      int64_t increment;
      SequenceVector::GetSequence(source, start, increment);
      Vector flattened(source.GetLogicalType());
      VectorOperations::GenerateSequence(flattened, source_count, start, increment);
      VectorOperations::Copy(flattened, target, FlatVector::INCREMENTAL_SELECTION_VECTOR, nullptr, source_count,
                             source_offset, target_offset);
      break;
    }
    case VectorType::SEQUENCE_CIRCULAR_VECTOR: {
      int64_t start;
      int64_t offset;
      int64_t stride;
      int64_t end;
      CircularSequenceVector::GetSequence(source, start, offset, stride, end);
      Vector flattened(source.GetLogicalType());
      VectorOperations::GenerateSequence(flattened, source_count, start, offset, stride, end);
      VectorOperations::Copy(flattened, target, FlatVector::INCREMENTAL_SELECTION_VECTOR, nullptr, source_count,
                             source_offset, target_offset);
      break;
    }
  }
}

void VectorOperations::Copy(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
                            idx_t source_offset, idx_t target_offset) {
  VectorOperations::Copy(source, target, sel, nullptr, source_count, source_offset, target_offset);
}

void VectorOperations::Copy(const Vector &source, Vector &target, const SelectionVector &sel,
                            const SelectionVector *target_sel, idx_t source_count, idx_t source_offset,
                            idx_t target_offset) {
  BUMBLEBEE_ASSERT(source_offset <= source_count, "VectorOperations::Copy: the source offset is past the source");
  BUMBLEBEE_ASSERT(source.GetType() == target.GetType(),
                   "VectorOperations::Copy: the source and the target have different types");
  idx_t copy_count = source_count - source_offset;

  switch (source.GetVectorType()) {
    case VectorType::DICTIONARY_VECTOR: {
      // Compose the two selections and recurse until the source is flat.
      auto &child = DictionaryVector::Child(source);
      auto &dict_sel = DictionaryVector::SelVector(source);
      auto new_buffer = dict_sel.Slice(sel, source_count);
      SelectionVector merged_sel(std::move(new_buffer));
      VectorOperations::Copy(child, target, merged_sel, target_sel, source_count, source_offset, target_offset);
      return;
    }
    case VectorType::SEQUENCE_VECTOR: {
      int64_t start;
      int64_t increment;
      SequenceVector::GetSequence(source, start, increment);
      Vector seq(source.GetLogicalType());
      VectorOperations::GenerateSequence(seq, source_count, sel, start, increment);
      VectorOperations::Copy(seq, target, sel, target_sel, source_count, source_offset, target_offset);
      return;
    }
    case VectorType::SEQUENCE_CIRCULAR_VECTOR: {
      int64_t start;
      int64_t offset;
      int64_t stride;
      int64_t end;
      CircularSequenceVector::GetSequence(source, start, offset, stride, end);
      Vector seq(source.GetLogicalType());
      VectorOperations::GenerateSequence(seq, source_count, sel, start, offset, stride, end);
      VectorOperations::Copy(seq, target, sel, target_sel, source_count, source_offset, target_offset);
      return;
    }
    case VectorType::CONSTANT_VECTOR:
      // `sel` should be a zero selection: every row reads the constant at row 0.
      break;
    case VectorType::FLAT_VECTOR:
      break;
  }

  if (copy_count == 0) {
    return;
  }

  // A single value may be copied into a constant target.
  const auto target_vtype = target.GetVectorType();
  if (copy_count == 1 && target_vtype == VectorType::CONSTANT_VECTOR) {
    target_offset = 0;
    target.SetVectorType(VectorType::FLAT_VECTOR);
  }
  BUMBLEBEE_ASSERT(target.GetVectorType() == VectorType::FLAT_VECTOR,
                   "VectorOperations::Copy: the target must be a flat vector");

  switch (source.GetType()) {
    case PhysicalType::TINYINT:
    // UNKNOWN (the untyped NULL literal) is an all-NULL vector over a 1-byte fill: copying its
    // bytes like a TINYINT moves the fill, and the validity copy below carries the actual NULLs.
    case PhysicalType::UNKNOWN:
      TemplatedCopyTargetSelSwitch<int8_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::SMALLINT:
      TemplatedCopyTargetSelSwitch<int16_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::INTEGER:
      TemplatedCopyTargetSelSwitch<int32_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::BIGINT:
      TemplatedCopyTargetSelSwitch<int64_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::UTINYINT:
      TemplatedCopyTargetSelSwitch<uint8_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::USMALLINT:
      TemplatedCopyTargetSelSwitch<uint16_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::UINTEGER:
      TemplatedCopyTargetSelSwitch<uint32_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::UBIGINT:
      TemplatedCopyTargetSelSwitch<uint64_t>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::FLOAT:
      TemplatedCopyTargetSelSwitch<float>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::DOUBLE:
      TemplatedCopyTargetSelSwitch<double>(source, sel, target, target_sel, source_offset, target_offset, copy_count);
      break;
    case PhysicalType::STRING: {
      // The bytes belong to the source's heap: copy them into the target's, so that the
      // target does not outlive the strings it points at.
      const auto *ldata = FlatVector::GetData<string_t>(source);
      auto *tdata = FlatVector::GetData<string_t>(target);
      const SelectionVector *tsel = target_sel != nullptr ? target_sel : &FlatVector::INCREMENTAL_SELECTION_VECTOR;
      for (idx_t i = 0; i < copy_count; i++) {
        auto source_idx = sel.GetIndex(source_offset + i);
        auto target_idx = tsel->GetIndex(target_offset + i);
        tdata[target_idx] = StringVector::AddString(target, ldata[source_idx]);
      }
      break;
    }
    case PhysicalType::LIST: {
      // The elements belong to the SOURCE's child, and the source's entries are offsets
      // into it. Copying the entries verbatim would leave the target pointing into memory
      // it does not own — it would read correctly right up until the source is reused or
      // freed, and then read garbage. So: append each row's elements to the TARGET's child,
      // and REMAP the entry to where they landed.
      const auto *ldata = FlatVector::GetData<ListEntry>(source);
      auto *tdata = FlatVector::GetData<ListEntry>(target);
      const SelectionVector *tsel = target_sel != nullptr ? target_sel : &FlatVector::INCREMENTAL_SELECTION_VECTOR;
      for (idx_t i = 0; i < copy_count; i++) {
        auto source_idx = sel.GetIndex(source_offset + i);
        auto target_idx = tsel->GetIndex(target_offset + i);
        const auto entry = ldata[source_idx];
        const idx_t new_offset = ListVector::Append(target, ListVector::GetChild(source),
                                                    entry.offset_ + entry.length_, entry.offset_);
        tdata[target_idx] = ListEntry{new_offset, entry.length_};
      }
      break;
    }
    case PhysicalType::ARRAY: {
      // No entries to remap: an ARRAY row is the child slice [i * n, (i + 1) * n), so the
      // copy is just the same copy, one level down, at the scaled indices.
      const idx_t array_size = ArrayVector::GetArraySize(source);
      BUMBLEBEE_ASSERT(array_size == ArrayVector::GetArraySize(target),
                       "VectorOperations::Copy: the two ARRAYs have different sizes");
      const auto &source_child = ArrayVector::GetChild(source);
      auto &target_child = ArrayVector::GetChild(target);
      const SelectionVector *tsel = target_sel != nullptr ? target_sel : &FlatVector::INCREMENTAL_SELECTION_VECTOR;
      for (idx_t i = 0; i < copy_count; i++) {
        auto source_idx = sel.GetIndex(source_offset + i);
        auto target_idx = tsel->GetIndex(target_offset + i);
        VectorOperations::Copy(source_child, target_child, (source_idx + 1) * array_size, source_idx * array_size,
                               target_idx * array_size);
      }
      break;
    }
    default:
      throw NotImplementedException(
          fmt::format("VectorOperations::Copy: unsupported type {}", LogicalType::NameOf(source.GetType())));
  }

  // Mirror the data copy with the validity: rows [target_offset, target_offset + copy_count)
  // must reflect the source's validity exactly — including CLEARING stale invalid bits the
  // target may carry from a previous use (a result chunk reused across iterations).
  //
  // Fast path: when both sides are all-valid there is nothing to do, since a target row is
  // valid by default and no source row can invalidate it.
  const auto &src_validity = source.Validity();
  const bool src_all_valid = src_validity.AllValid();
  if (!src_all_valid || !target.Validity().AllValid()) {
    const SelectionVector *tsel = target_sel != nullptr ? target_sel : &FlatVector::INCREMENTAL_SELECTION_VECTOR;
    // The target is FLAT: grow its mask once, up to the last row we are about to write.
    ValidityMask &tmask = FlatVector::Validity(target);
    tmask.EnsureWritable(target_offset + copy_count);
    for (idx_t i = 0; i < copy_count; i++) {
      auto source_idx = sel.GetIndex(source_offset + i);
      auto target_idx = tsel->GetIndex(target_offset + i);
      if (src_all_valid || src_validity.RowIsValid(source_idx)) {
        tmask.SetValidUnsafe(target_idx);
      } else {
        tmask.SetInvalidUnsafe(target_idx);
      }
    }
  }

  if (target_vtype != VectorType::FLAT_VECTOR) {
    target.SetVectorType(target_vtype);
  }
}

}  // namespace bumblebee
