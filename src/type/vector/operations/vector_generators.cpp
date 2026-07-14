//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_generators.cpp
//
// Identification: src/type/vector/operations/vector_generators.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/exception.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/** @brief Materialize start, start + increment, ... over the first `count` rows. */
template <class T>
static void TemplatedGenerateSequence(Vector &result, idx_t count, int64_t start, int64_t increment) {
  BUMBLEBEE_ASSERT(result.GetType() != PhysicalType::STRING, "a sequence vector cannot hold strings");
  result.SetVectorType(VectorType::FLAT_VECTOR);
  auto *result_data = FlatVector::GetData<T>(result);
  auto value = static_cast<T>(start);
  for (idx_t i = 0; i < count; i++) {
    result_data[i] = value;
    value = static_cast<T>(value + increment);
  }
}

/** @brief Materialize the circular sequence over [start, end] on the first `count` rows. */
template <class T>
static void TemplatedGenerateSequence(Vector &result, idx_t count, int64_t start, int64_t offset, int64_t stride,
                                      int64_t end) {
  BUMBLEBEE_ASSERT(result.GetType() != PhysicalType::STRING, "a sequence vector cannot hold strings");
  BUMBLEBEE_ASSERT(start < end, "a circular sequence needs start < end");
  result.SetVectorType(VectorType::FLAT_VECTOR);
  auto *result_data = FlatVector::GetData<T>(result);
  int64_t size = end - start + 1;
  for (idx_t i = 0; i < count; i++) {
    result_data[i] = static_cast<T>(start) + static_cast<T>((static_cast<int64_t>(i) + offset) / stride % size);
  }
}

/** @brief Materialize start + idx * increment, but only at the rows named by `sel`. */
template <class T>
static void TemplatedGenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start,
                                      int64_t increment) {
  BUMBLEBEE_ASSERT(result.GetType() != PhysicalType::STRING, "a sequence vector cannot hold strings");
  result.SetVectorType(VectorType::FLAT_VECTOR);
  auto *result_data = FlatVector::GetData<T>(result);
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel.GetIndex(i);
    // Note: the value written is the one the sequence has AT ROW `idx`, so that reading
    // result_data[sel[i]] gives the same answer as reading the sequence at sel[i].
    result_data[idx] = static_cast<T>(start + static_cast<int64_t>(idx) * increment);
  }
}

/** @brief Materialize the circular sequence, but only at the rows named by `sel`. */
template <class T>
static void TemplatedGenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start,
                                      int64_t offset, int64_t stride, int64_t end) {
  BUMBLEBEE_ASSERT(result.GetType() != PhysicalType::STRING, "a sequence vector cannot hold strings");
  BUMBLEBEE_ASSERT(start < end, "a circular sequence needs start < end");
  result.SetVectorType(VectorType::FLAT_VECTOR);
  auto *result_data = FlatVector::GetData<T>(result);
  int64_t size = end - start + 1;
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel.GetIndex(i);
    result_data[idx] = static_cast<T>(start) + static_cast<T>((offset + static_cast<int64_t>(idx)) / stride % size);
  }
}

template <class T, bool HAS_END>
static void TemplatedGeneralGenerateSequence(Vector &result, idx_t count, int64_t start, int64_t offset, int64_t stride,
                                             int64_t increment, int64_t end) {
  if constexpr (HAS_END) {
    TemplatedGenerateSequence<T>(result, count, start, offset, stride, end);
  } else {
    TemplatedGenerateSequence<T>(result, count, start, increment);
  }
}

template <class T, bool HAS_END>
static void TemplatedGeneralGenerateSelectionSequence(Vector &result, idx_t count, const SelectionVector &sel,
                                                      int64_t start, int64_t offset, int64_t stride, int64_t increment,
                                                      int64_t end) {
  if constexpr (HAS_END) {
    TemplatedGenerateSequence<T>(result, count, sel, start, offset, stride, end);
  } else {
    TemplatedGenerateSequence<T>(result, count, sel, start, increment);
  }
}

/** @brief Dispatch the sequence generation on the physical type of `result`. */
template <bool HAS_END, bool HAS_SEL>
static void SwitchGenerateSequence(Vector &result, idx_t count, const SelectionVector *sel, int64_t start,
                                   int64_t offset, int64_t stride, int64_t increment, int64_t end) {
  auto generate = [&]<class T>() {
    if constexpr (HAS_SEL) {
      TemplatedGeneralGenerateSelectionSequence<T, HAS_END>(result, count, *sel, start, offset, stride, increment, end);
    } else {
      TemplatedGeneralGenerateSequence<T, HAS_END>(result, count, start, offset, stride, increment, end);
    }
  };
  switch (result.GetType()) {
    case PhysicalType::TINYINT:
      generate.template operator()<int8_t>();
      break;
    case PhysicalType::SMALLINT:
      generate.template operator()<int16_t>();
      break;
    case PhysicalType::INTEGER:
      generate.template operator()<int32_t>();
      break;
    case PhysicalType::BIGINT:
      generate.template operator()<int64_t>();
      break;
    case PhysicalType::UTINYINT:
      generate.template operator()<uint8_t>();
      break;
    case PhysicalType::USMALLINT:
      generate.template operator()<uint16_t>();
      break;
    case PhysicalType::UINTEGER:
      generate.template operator()<uint32_t>();
      break;
    case PhysicalType::UBIGINT:
      generate.template operator()<uint64_t>();
      break;
    case PhysicalType::FLOAT:
      generate.template operator()<float>();
      break;
    case PhysicalType::DOUBLE:
      generate.template operator()<double>();
      break;
    default:
      throw NotImplementedException(
          fmt::format("GenerateSequence: unsupported type {}", LogicalType::NameOf(result.GetType())));
  }
}

void VectorOperations::GenerateSequence(Vector &result, idx_t count, int64_t start, int64_t increment) {
  SwitchGenerateSequence<false, false>(result, count, nullptr, start, 0, 0, increment, 0);
}

void VectorOperations::GenerateSequence(Vector &result, idx_t count, int64_t start, int64_t offset, int64_t stride,
                                        int64_t end) {
  SwitchGenerateSequence<true, false>(result, count, nullptr, start, offset, stride, 0, end);
}

void VectorOperations::GenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start,
                                        int64_t increment) {
  SwitchGenerateSequence<false, true>(result, count, &sel, start, 0, 0, increment, 0);
}

void VectorOperations::GenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start,
                                        int64_t offset, int64_t stride, int64_t end) {
  SwitchGenerateSequence<true, true>(result, count, &sel, start, offset, stride, 0, end);
}

}  // namespace bumblebee
