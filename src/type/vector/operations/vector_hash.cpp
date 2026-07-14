//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_hash.cpp
//
// Identification: src/type/vector/operations/vector_hash.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/exception.h"
#include "common/hash.h"
#include "common/macros.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

namespace {

/**
 * @brief Hash `count` rows, writing each hash at the row's own index.
 *
 * @tparam HAS_RSEL Whether only the rows named by `rsel` are to be hashed. Note the hash
 *         still lands at the SELECTED index, not at i — the caller reads the result
 *         through the same selection.
 */
template <bool HAS_RSEL, class T>
inline void TightLoopHash(T *__restrict ldata, hash_t *__restrict rdata, const SelectionVector *rsel, idx_t count,
                          const SelectionVector *__restrict lsel) {
  for (idx_t i = 0; i < count; i++) {
    auto ridx = HAS_RSEL ? rsel->GetIndex(i) : i;
    auto lidx = lsel->GetIndex(ridx);
    rdata[ridx] = Hash<T>(ldata[lidx]);
  }
}

template <bool HAS_RSEL, class T>
inline void TemplatedLoopHash(Vector &input, Vector &result, const SelectionVector *rsel, idx_t count) {
  if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
    // One value, so one hash: the result stays a constant.
    result.SetVectorType(VectorType::CONSTANT_VECTOR);

    auto *ldata = ConstantVector::GetData<T>(input);
    auto *result_data = ConstantVector::GetData<hash_t>(result);
    *result_data = Hash<T>(*ldata);
  } else {
    result.SetVectorType(VectorType::FLAT_VECTOR);

    VectorData idata;
    input.Orrify(count, idata);

    TightLoopHash<HAS_RSEL, T>(reinterpret_cast<T *>(idata.data_), FlatVector::GetData<hash_t>(result), rsel, count,
                               idata.sel_);
  }
}

/**
 * @brief Fold two hashes into one.
 *
 * The multiply scrambles the accumulated hash before the XOR, so that combining the same
 * two column hashes in a different order — or hashing (a, b) versus (b, a) — does not
 * collide, which a bare XOR would.
 */
inline auto CombineHashScalar(hash_t a, hash_t b) -> hash_t { return (a * UINT64_C(0xbf58476d1ce4e5b9)) ^ b; }

/** The hash of a NULL element inside a list. Distinct from the hash of any real element. */
constexpr hash_t NESTED_NULL_HASH = UINT64_C(0x9e3779b97f4a7c15);

/**
 * @brief Hash one row of any vector, recursing into a LIST / ARRAY element by element.
 *
 * Two lists holding the same elements in the same order hash the same, and the length is
 * folded in first so that [1] and [1, 1] do not collide. A NULL element hashes to its own
 * constant, which is what keeps [NULL] apart from an empty list.
 */
auto HashNestedRow(const Vector &vector, idx_t index) -> hash_t {
  switch (vector.GetVectorType()) {
    case VectorType::CONSTANT_VECTOR:
      index = 0;
      break;
    case VectorType::DICTIONARY_VECTOR:
      return HashNestedRow(DictionaryVector::Child(vector),
                           DictionaryVector::SelVector(vector).GetIndex(index));
    case VectorType::FLAT_VECTOR:
      break;
    default:
      throw NotImplementedException("hash: a nested vector cannot hold a sequence");
  }
  if (!vector.RowIsValid(index)) {
    return NESTED_NULL_HASH;
  }
  switch (vector.GetType()) {
    case PhysicalType::TINYINT:
      return Hash<int8_t>(FlatVector::GetData<int8_t>(vector)[index]);
    case PhysicalType::SMALLINT:
      return Hash<int16_t>(FlatVector::GetData<int16_t>(vector)[index]);
    case PhysicalType::INTEGER:
      return Hash<int32_t>(FlatVector::GetData<int32_t>(vector)[index]);
    case PhysicalType::BIGINT:
      return Hash<int64_t>(FlatVector::GetData<int64_t>(vector)[index]);
    case PhysicalType::UTINYINT:
      return Hash<uint8_t>(FlatVector::GetData<uint8_t>(vector)[index]);
    case PhysicalType::USMALLINT:
      return Hash<uint16_t>(FlatVector::GetData<uint16_t>(vector)[index]);
    case PhysicalType::UINTEGER:
      return Hash<uint32_t>(FlatVector::GetData<uint32_t>(vector)[index]);
    case PhysicalType::UBIGINT:
      return Hash<uint64_t>(FlatVector::GetData<uint64_t>(vector)[index]);
    case PhysicalType::FLOAT:
      return Hash<float>(FlatVector::GetData<float>(vector)[index]);
    case PhysicalType::DOUBLE:
      return Hash<double>(FlatVector::GetData<double>(vector)[index]);
    case PhysicalType::STRING:
      return Hash<string_t>(FlatVector::GetData<string_t>(vector)[index]);
    case PhysicalType::LIST: {
      const auto &entry = ListVector::GetEntries(vector)[index];
      const auto &child = ListVector::GetChild(vector);
      hash_t result = Hash<uint64_t>(entry.length_);
      for (idx_t i = 0; i < entry.length_; i++) {
        result = CombineHashScalar(result, HashNestedRow(child, entry.offset_ + i));
      }
      return result;
    }
    case PhysicalType::ARRAY: {
      const idx_t array_size = ArrayVector::GetArraySize(vector);
      const auto &child = ArrayVector::GetChild(vector);
      hash_t result = Hash<uint64_t>(array_size);
      for (idx_t i = 0; i < array_size; i++) {
        result = CombineHashScalar(result, HashNestedRow(child, index * array_size + i));
      }
      return result;
    }
    default:
      throw NotImplementedException(fmt::format("hash: unsupported type {}", LogicalType::NameOf(vector.GetType())));
  }
}

/** @brief Hash a LIST / ARRAY vector row by row. The result is always FLAT. */
template <bool HAS_RSEL>
void LoopHashNested(Vector &input, Vector &result, const SelectionVector *rsel, idx_t count) {
  result.SetVectorType(VectorType::FLAT_VECTOR);
  auto *result_data = FlatVector::GetData<hash_t>(result);
  for (idx_t i = 0; i < count; i++) {
    auto ridx = HAS_RSEL ? rsel->GetIndex(i) : i;
    result_data[ridx] = HashNestedRow(input, ridx);
  }
}

template <bool HAS_RSEL>
inline void HashTypeSwitch(Vector &input, Vector &result, const SelectionVector *rsel, idx_t count) {
  BUMBLEBEE_ASSERT(result.GetType() == PhysicalType::UBIGINT, "the hash result must be a UBIGINT vector");

  switch (input.GetType()) {
    case PhysicalType::TINYINT:
      TemplatedLoopHash<HAS_RSEL, int8_t>(input, result, rsel, count);
      break;
    case PhysicalType::SMALLINT:
      TemplatedLoopHash<HAS_RSEL, int16_t>(input, result, rsel, count);
      break;
    case PhysicalType::INTEGER:
      TemplatedLoopHash<HAS_RSEL, int32_t>(input, result, rsel, count);
      break;
    case PhysicalType::BIGINT:
      TemplatedLoopHash<HAS_RSEL, int64_t>(input, result, rsel, count);
      break;
    case PhysicalType::UTINYINT:
      TemplatedLoopHash<HAS_RSEL, uint8_t>(input, result, rsel, count);
      break;
    case PhysicalType::USMALLINT:
      TemplatedLoopHash<HAS_RSEL, uint16_t>(input, result, rsel, count);
      break;
    case PhysicalType::UINTEGER:
      TemplatedLoopHash<HAS_RSEL, uint32_t>(input, result, rsel, count);
      break;
    case PhysicalType::UBIGINT:
      TemplatedLoopHash<HAS_RSEL, uint64_t>(input, result, rsel, count);
      break;
    case PhysicalType::FLOAT:
      TemplatedLoopHash<HAS_RSEL, float>(input, result, rsel, count);
      break;
    case PhysicalType::DOUBLE:
      TemplatedLoopHash<HAS_RSEL, double>(input, result, rsel, count);
      break;
    case PhysicalType::STRING:
      TemplatedLoopHash<HAS_RSEL, string_t>(input, result, rsel, count);
      break;
    case PhysicalType::LIST:
    case PhysicalType::ARRAY:
      LoopHashNested<HAS_RSEL>(input, result, rsel, count);
      break;
    default:
      throw NotImplementedException(fmt::format("hash: unsupported type {}", LogicalType::NameOf(input.GetType())));
  }
}

template <bool HAS_RSEL, class T>
inline void TightLoopCombineHashConstant(T *__restrict ldata, hash_t constant_hash, hash_t *__restrict hash_data,
                                         const SelectionVector *rsel, idx_t count,
                                         const SelectionVector *__restrict lsel) {
  for (idx_t i = 0; i < count; i++) {
    auto ridx = HAS_RSEL ? rsel->GetIndex(i) : i;
    auto idx = lsel->GetIndex(ridx);
    auto other_hash = Hash<T>(ldata[idx]);
    hash_data[ridx] = CombineHashScalar(constant_hash, other_hash);
  }
}

template <bool HAS_RSEL, class T>
inline void TightLoopCombineHash(T *__restrict ldata, hash_t *__restrict hash_data, const SelectionVector *rsel,
                                 idx_t count, const SelectionVector *__restrict lsel) {
  for (idx_t i = 0; i < count; i++) {
    auto ridx = HAS_RSEL ? rsel->GetIndex(i) : i;
    auto idx = lsel->GetIndex(ridx);
    auto other_hash = Hash<T>(ldata[idx]);
    hash_data[ridx] = CombineHashScalar(hash_data[ridx], other_hash);
  }
}

template <bool HAS_RSEL, class T>
void TemplatedLoopCombineHash(Vector &input, Vector &hashes, const SelectionVector *rsel, idx_t count) {
  if (input.GetVectorType() == VectorType::CONSTANT_VECTOR &&
      hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
    // Both sides are a single value, so the combination is too.
    auto *ldata = ConstantVector::GetData<T>(input);
    auto *hash = ConstantVector::GetData<hash_t>(hashes);
    auto other_hash = Hash<T>(*ldata);
    *hash = CombineHashScalar(*hash, other_hash);
    return;
  }

  VectorData idata;
  input.Orrify(count, idata);

  if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
    // The accumulated hash is a single value but the input is not, so the result has to
    // become flat. Read the constant out FIRST — flattening overwrites the slot it lives in.
    auto hash = *ConstantVector::GetData<hash_t>(hashes);
    hashes.SetVectorType(VectorType::FLAT_VECTOR);
    TightLoopCombineHashConstant<HAS_RSEL, T>(reinterpret_cast<T *>(idata.data_), hash,
                                              FlatVector::GetData<hash_t>(hashes), rsel, count, idata.sel_);
    return;
  }
  BUMBLEBEE_ASSERT(hashes.GetVectorType() == VectorType::FLAT_VECTOR, "the hash accumulator must be flat");
  TightLoopCombineHash<HAS_RSEL, T>(reinterpret_cast<T *>(idata.data_), FlatVector::GetData<hash_t>(hashes), rsel,
                                    count, idata.sel_);
}

/** @brief Fold the hash of every row of a LIST / ARRAY vector into `hashes`. */
template <bool HAS_RSEL>
void LoopCombineHashNested(Vector &input, Vector &hashes, const SelectionVector *rsel, idx_t count) {
  if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
    // Read the accumulated constant out FIRST: turning the accumulator flat overwrites the
    // slot it lives in.
    auto hash = *ConstantVector::GetData<hash_t>(hashes);
    hashes.SetVectorType(VectorType::FLAT_VECTOR);
    auto *hash_data = FlatVector::GetData<hash_t>(hashes);
    for (idx_t i = 0; i < count; i++) {
      auto ridx = HAS_RSEL ? rsel->GetIndex(i) : i;
      hash_data[ridx] = CombineHashScalar(hash, HashNestedRow(input, ridx));
    }
    return;
  }
  BUMBLEBEE_ASSERT(hashes.GetVectorType() == VectorType::FLAT_VECTOR, "the hash accumulator must be flat");
  auto *hash_data = FlatVector::GetData<hash_t>(hashes);
  for (idx_t i = 0; i < count; i++) {
    auto ridx = HAS_RSEL ? rsel->GetIndex(i) : i;
    hash_data[ridx] = CombineHashScalar(hash_data[ridx], HashNestedRow(input, ridx));
  }
}

template <bool HAS_RSEL>
inline void CombineHashTypeSwitch(Vector &hashes, Vector &input, const SelectionVector *rsel, idx_t count) {
  BUMBLEBEE_ASSERT(hashes.GetType() == PhysicalType::UBIGINT, "the hash accumulator must be a UBIGINT vector");

  switch (input.GetType()) {
    case PhysicalType::TINYINT:
      TemplatedLoopCombineHash<HAS_RSEL, int8_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::SMALLINT:
      TemplatedLoopCombineHash<HAS_RSEL, int16_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::INTEGER:
      TemplatedLoopCombineHash<HAS_RSEL, int32_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::BIGINT:
      TemplatedLoopCombineHash<HAS_RSEL, int64_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::UTINYINT:
      TemplatedLoopCombineHash<HAS_RSEL, uint8_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::USMALLINT:
      TemplatedLoopCombineHash<HAS_RSEL, uint16_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::UINTEGER:
      TemplatedLoopCombineHash<HAS_RSEL, uint32_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::UBIGINT:
      TemplatedLoopCombineHash<HAS_RSEL, uint64_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::FLOAT:
      TemplatedLoopCombineHash<HAS_RSEL, float>(input, hashes, rsel, count);
      break;
    case PhysicalType::DOUBLE:
      TemplatedLoopCombineHash<HAS_RSEL, double>(input, hashes, rsel, count);
      break;
    case PhysicalType::STRING:
      TemplatedLoopCombineHash<HAS_RSEL, string_t>(input, hashes, rsel, count);
      break;
    case PhysicalType::LIST:
    case PhysicalType::ARRAY:
      LoopCombineHashNested<HAS_RSEL>(input, hashes, rsel, count);
      break;
    default:
      throw NotImplementedException(
          fmt::format("combine hash: unsupported type {}", LogicalType::NameOf(input.GetType())));
  }
}

}  // namespace

void VectorOperations::Hash(Vector &input, Vector &hashes, idx_t count) {
  HashTypeSwitch<false>(input, hashes, nullptr, count);
}

void VectorOperations::Hash(Vector &input, Vector &hashes, const SelectionVector &rsel, idx_t count) {
  HashTypeSwitch<true>(input, hashes, &rsel, count);
}

void VectorOperations::CombineHash(Vector &hashes, Vector &input, idx_t count) {
  CombineHashTypeSwitch<false>(hashes, input, nullptr, count);
}

void VectorOperations::CombineHash(Vector &hashes, Vector &input, const SelectionVector &rsel, idx_t count) {
  CombineHashTypeSwitch<true>(hashes, input, &rsel, count);
}

}  // namespace bumblebee
