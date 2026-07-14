//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// create_sort_key.cpp
//
// Identification: src/type/vector/operations/create_sort_key.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/operations/create_sort_key.h"

#include <memory>
#include <vector>

#include "common/exception.h"
#include "common/helper.h"
#include "common/macros.h"
#include "common/sort_key_encoding.h"
#include "type/bumble_string.h"

namespace bumblebee {

namespace {

/** One column to encode, and how many rows of it. */
struct SortKeyVectorData {
  SortKeyVectorData(Vector &vector, idx_t size) : vector_(vector), size_(size) {}

  Vector &vector_;
  idx_t size_;
};

using sort_key_data_ptr_t = std::unique_ptr<SortKeyVectorData>;

/**
 * How many bytes the keys need.
 *
 * The fixed-width columns contribute the same number of bytes to every row, so they are
 * summed once into `constant_`. Only the STRING columns need a per-row tally.
 */
struct SortKeyLengthInfo {
  explicit SortKeyLengthInfo(idx_t size) : constant_(0) { variable_.resize(size, 0); }

  idx_t constant_;
  std::vector<idx_t> variable_;
};

/** @brief The encoder for a fixed-width type. */
template <class T>
struct SortKeyConstantOperator {
  using TYPE = T;

  static auto GetEncodeLength(T input) -> idx_t {
    (void)input;
    return sizeof(T);
  }

  static auto Encode(data_ptr_t result, T input) -> idx_t {
    SortKeyEncoding::EncodeData<T>(result, input);
    return sizeof(T);
  }

  /**
   * @brief Encode a NULL: all-0xFF bytes, of the type's natural width.
   *
   * 0xFF is above every real encoded value, so a NULL sorts LAST in ASC. The DESC pass
   * flips every byte, turning it into all-0x00 — which sorts FIRST. That is exactly the
   * conventional SQL default (NULLS LAST in ASC, NULLS FIRST in DESC), for free.
   */
  static auto EncodeNull(data_ptr_t result) -> idx_t {
    for (idx_t i = 0; i < sizeof(T); i++) {
      result[i] = 0xFF;
    }
    return sizeof(T);
  }
};

/** @brief The encoder for a STRING. */
struct SortKeyStringOperator {
  static constexpr data_t STRING_DELIMITER = 0;
  /**
   * Every STRING key opens with a marker byte: NON_NULL_PREFIX for a real value,
   * NON_NULL_PREFIX + 1 for a NULL. Both sit above STRING_DELIMITER (0x00), and the +1 gap
   * means a non-null key always compares strictly below a NULL key under memcmp — NULLS
   * LAST in ASC. The whole-key flip for DESC turns them into 0xFE / 0xFD, putting the NULLs
   * first there. The string's own bytes are shifted up by one so that none of them can
   * collide with the terminating delimiter.
   */
  static constexpr data_t NON_NULL_PREFIX = 0x01;
  using TYPE = string_t;

  /** @return The encoded length: the bytes, plus the marker, plus the delimiter. */
  static auto GetEncodeLength(TYPE &input) -> idx_t { return input.Size() + 2; }

  static auto Encode(data_ptr_t result, TYPE &input) -> idx_t {
    const auto *input_data = reinterpret_cast<const_data_ptr_t>(input.GetDataUnsafe());
    auto input_size = input.Size();
    result[0] = NON_NULL_PREFIX;
    for (idx_t r = 0; r < input_size; r++) {
      result[1 + r] = input_data[r] + 1;
    }
    result[1 + input_size] = STRING_DELIMITER;
    return input_size + 2;
  }

  /** @return The encoded length of a NULL: the marker byte, and nothing else. */
  static auto GetNullEncodeLength() -> idx_t { return 1; }

  static auto EncodeNull(data_ptr_t result) -> idx_t {
    result[0] = NON_NULL_PREFIX + 1;
    return 1;
  }
};

/** Where each row's key lives, how far into it we have written, and whether to flip. */
struct SortKeyConstructInfo {
  SortKeyConstructInfo(OrderModifiers modifiers, std::vector<idx_t> &offsets, data_ptr_t *result)
      : modifiers_(modifiers), result_(result), offsets_(offsets) {
    flip_bytes_ = modifiers.order_type_ == OrderType::DESCENDING;
  }

  OrderModifiers modifiers_;
  data_ptr_t *result_;
  std::vector<idx_t> &offsets_;
  bool flip_bytes_;
};

/**
 * @brief Encode one row's value (or its NULL marker), apply the DESC flip, advance the offset.
 *
 * DESCENDING is not a separate encoding: it is the ascending encoding with every byte
 * complemented, which exactly reverses the memcmp order.
 */
template <class OP, class T>
inline void EncodeOneRow(data_ptr_t result_ptr, idx_t &offset, T value, bool is_null, bool flip) {
  idx_t encode_len = is_null ? OP::EncodeNull(result_ptr + offset) : OP::Encode(result_ptr + offset, value);

  if (flip) {
    for (idx_t b = offset; b < offset + encode_len; b++) {
      result_ptr[b] = ~result_ptr[b];
    }
  }
  offset += encode_len;
}

template <class OP>
void TemplatedConstructSortKeyConstant(Vector &vector, idx_t size, SortKeyConstructInfo &info) {
  BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR, "expected a constant vector");
  auto *data = ConstantVector::GetData<typename OP::TYPE>(vector);
  bool is_null = !vector.RowIsValid(0);
  for (idx_t r = 0; r < size; r++) {
    EncodeOneRow<OP>(info.result_[r], info.offsets_[r], data[0], is_null, info.flip_bytes_);
  }
}

template <class OP, class T, bool HAS_NULL>
void TemplatedConstructSortKeyFlat(T *__restrict data, const ValidityMask &validity, idx_t size,
                                   data_ptr_t *__restrict result, idx_t *__restrict offsets, bool flip) {
  if (HAS_NULL) {
    bool no_nulls = validity.AllValid();
    for (idx_t r = 0; r < size; r++) {
      bool is_null = !no_nulls && !validity.RowIsValid(r);
      EncodeOneRow<OP>(result[r], offsets[r], data[r], is_null, flip);
    }
  } else {
    for (idx_t r = 0; r < size; r++) {
      EncodeOneRow<OP>(result[r], offsets[r], data[r], false, flip);
    }
  }
}

template <class OP, class T, bool HAS_NULL>
void TemplatedConstructSortKeyGeneric(T *__restrict data, const ValidityMask *validity, idx_t size,
                                      const SelectionVector &sel, data_ptr_t *__restrict result,
                                      idx_t *__restrict offsets, bool flip) {
  if (HAS_NULL) {
    bool no_nulls = validity == nullptr || validity->AllValid();
    for (idx_t r = 0; r < size; r++) {
      idx_t idx = sel.GetIndex(r);
      bool is_null = !no_nulls && !validity->RowIsValid(idx);
      EncodeOneRow<OP>(result[r], offsets[r], data[idx], is_null, flip);
    }
  } else {
    for (idx_t r = 0; r < size; r++) {
      idx_t idx = sel.GetIndex(r);
      EncodeOneRow<OP>(result[r], offsets[r], data[idx], false, flip);
    }
  }
}

template <class OP>
void TemplatedConstructSortKey(SortKeyVectorData &vector_data, SortKeyConstructInfo &info) {
  auto &vector = vector_data.vector_;
  switch (vector.GetVectorType()) {
    case VectorType::CONSTANT_VECTOR:
      TemplatedConstructSortKeyConstant<OP>(vector, vector_data.size_, info);
      break;
    case VectorType::FLAT_VECTOR: {
      auto *data_ptr = FlatVector::GetData<typename OP::TYPE>(vector);
      if (vector.Validity().AllValid()) {
        TemplatedConstructSortKeyFlat<OP, typename OP::TYPE, false>(data_ptr, FlatVector::Validity(vector),
                                                                    vector_data.size_, info.result_,
                                                                    info.offsets_.data(), info.flip_bytes_);
      } else {
        TemplatedConstructSortKeyFlat<OP, typename OP::TYPE, true>(data_ptr, FlatVector::Validity(vector),
                                                                   vector_data.size_, info.result_,
                                                                   info.offsets_.data(), info.flip_bytes_);
      }
      break;
    }
    default: {
      VectorData vd;
      vector.Orrify(vector_data.size_, vd);
      if (vd.validity_->AllValid()) {
        TemplatedConstructSortKeyGeneric<OP, typename OP::TYPE, false>(
            reinterpret_cast<typename OP::TYPE *>(vd.data_), vd.validity_, vector_data.size_, *vd.sel_, info.result_,
            info.offsets_.data(), info.flip_bytes_);
      } else {
        TemplatedConstructSortKeyGeneric<OP, typename OP::TYPE, true>(
            reinterpret_cast<typename OP::TYPE *>(vd.data_), vd.validity_, vector_data.size_, *vd.sel_, info.result_,
            info.offsets_.data(), info.flip_bytes_);
      }
    }
  }
}

void ConstructSortKey(SortKeyVectorData &vector_data, SortKeyConstructInfo &info) {
  auto &vector = vector_data.vector_;
  switch (vector.GetType()) {
    case PhysicalType::TINYINT:
      TemplatedConstructSortKey<SortKeyConstantOperator<int8_t>>(vector_data, info);
      break;
    case PhysicalType::SMALLINT:
      TemplatedConstructSortKey<SortKeyConstantOperator<int16_t>>(vector_data, info);
      break;
    case PhysicalType::INTEGER:
      TemplatedConstructSortKey<SortKeyConstantOperator<int32_t>>(vector_data, info);
      break;
    case PhysicalType::BIGINT:
      TemplatedConstructSortKey<SortKeyConstantOperator<int64_t>>(vector_data, info);
      break;
    case PhysicalType::UTINYINT:
      TemplatedConstructSortKey<SortKeyConstantOperator<uint8_t>>(vector_data, info);
      break;
    case PhysicalType::USMALLINT:
      TemplatedConstructSortKey<SortKeyConstantOperator<uint16_t>>(vector_data, info);
      break;
    case PhysicalType::UINTEGER:
      TemplatedConstructSortKey<SortKeyConstantOperator<uint32_t>>(vector_data, info);
      break;
    case PhysicalType::UBIGINT:
      TemplatedConstructSortKey<SortKeyConstantOperator<uint64_t>>(vector_data, info);
      break;
    case PhysicalType::FLOAT:
      TemplatedConstructSortKey<SortKeyConstantOperator<float>>(vector_data, info);
      break;
    case PhysicalType::DOUBLE:
      TemplatedConstructSortKey<SortKeyConstantOperator<double>>(vector_data, info);
      break;
    case PhysicalType::STRING:
      TemplatedConstructSortKey<SortKeyStringOperator>(vector_data, info);
      break;
    default:
      throw NotImplementedException(
          fmt::format("create_sort_key: unsupported type {}", LogicalType::NameOf(vector.GetType())));
  }
}

/** @return The bytes one STRING row needs: the NULL marker, or the full encoding. */
inline auto StringRowEncodeLength(string_t &value, bool is_null) -> idx_t {
  return is_null ? SortKeyStringOperator::GetNullEncodeLength() : SortKeyStringOperator::GetEncodeLength(value);
}

void GetSortKeyVariableLengthGeneric(string_t *__restrict data, const ValidityMask *validity, idx_t size,
                                     const SelectionVector &sel, idx_t *__restrict result) {
  bool no_nulls = validity == nullptr || validity->AllValid();
  for (idx_t i = 0; i < size; ++i) {
    auto idx = sel.GetIndex(i);
    bool is_null = !no_nulls && !validity->RowIsValid(idx);
    result[i] += StringRowEncodeLength(data[idx], is_null);
  }
}

void GetSortKeyVariableLengthConstant(Vector &data, idx_t size, idx_t *__restrict result) {
  BUMBLEBEE_ASSERT(data.GetVectorType() == VectorType::CONSTANT_VECTOR, "expected a constant vector");
  auto *s = ConstantVector::GetData<string_t>(data);
  bool is_null = ConstantVector::IsNull(data);
  auto length = StringRowEncodeLength(*s, is_null);
  for (idx_t i = 0; i < size; ++i) {
    result[i] += length;
  }
}

void GetSortKeyVariableLengthFlat(string_t *__restrict data, const ValidityMask &validity, idx_t size,
                                  idx_t *__restrict result) {
  bool all_valid = validity.AllValid();
  for (idx_t i = 0; i < size; ++i) {
    bool is_null = !all_valid && !validity.RowIsValid(i);
    result[i] += StringRowEncodeLength(data[i], is_null);
  }
}

void GetSortKeyLength(SortKeyVectorData &data, SortKeyLengthInfo &result) {
  auto &vector = data.vector_;
  auto type = vector.GetType();

  if (LogicalType::IsConstantSize(type)) {
    // A fixed-width column takes the same number of bytes whether the row is NULL or not
    // (EncodeNull pads to sizeof(T)), so one addend covers every row of the column.
    result.constant_ += LogicalType::SizeOf(type);
    return;
  }

  if (type != PhysicalType::STRING) {
    throw NotImplementedException(
        fmt::format("create_sort_key: unsupported type {}", LogicalType::NameOf(type)));
  }
  switch (vector.GetVectorType()) {
    case VectorType::CONSTANT_VECTOR:
      GetSortKeyVariableLengthConstant(vector, data.size_, result.variable_.data());
      break;
    case VectorType::FLAT_VECTOR: {
      auto *data_ptr = FlatVector::GetData<string_t>(vector);
      GetSortKeyVariableLengthFlat(data_ptr, FlatVector::Validity(vector), data.size_, result.variable_.data());
      break;
    }
    default: {
      VectorData vd;
      vector.Orrify(data.size_, vd);
      GetSortKeyVariableLengthGeneric(reinterpret_cast<string_t *>(vd.data_), vd.validity_, data.size_, *vd.sel_,
                                      result.variable_.data());
    }
  }
}

/** @brief Reserve the bytes of every row's key in `result`'s heap, and record where they are. */
void PrepareSortData(Vector &result, idx_t size, SortKeyLengthInfo &key_lengths, data_ptr_t *data_ptr) {
  BUMBLEBEE_ASSERT(result.GetType() == PhysicalType::STRING, "the sort keys must go into a STRING vector");

  auto *result_data = FlatVector::GetData<string_t>(result);

  // Fast path: every key is the same size (no variable-length sort column) and does not fit
  // inline. Then one heap buffer can be carved into equal slots, instead of paying a heap
  // call per row. The resulting string_t values reference the same heap with identical
  // bytes, so comparisons and copies are byte-for-byte unchanged.
  bool fixed_width = true;
  for (idx_t r = 0; r < size; r++) {
    if (key_lengths.variable_[r] != 0) {
      fixed_width = false;
      break;
    }
  }
  const idx_t blob_size = key_lengths.constant_;
  if (fixed_width && blob_size > BumbleString::PREFIX_LENGTH) {
    const idx_t stride = blob_size + 1;  // +1 for the NUL terminator
    const idx_t per_buf = MaxValue<idx_t>(1, (MINIMUM_HEAP_SIZE - 1) / stride);
    idx_t r = 0;
    while (r < size) {
      idx_t batch = MinValue<idx_t>(per_buf, size - r);
      char *base = StringVector::EmptyString(result, batch * stride).GetDataWriteable();
      for (idx_t k = 0; k < batch; ++k, ++r) {
        char *p = base + k * stride;
        p[blob_size] = '\0';
        result_data[r] = string_t(p, static_cast<uint32_t>(blob_size));
        data_ptr[r] = reinterpret_cast<data_ptr_t>(p);
      }
    }
    return;
  }

  for (idx_t r = 0; r < size; r++) {
    auto bs = key_lengths.variable_[r] + key_lengths.constant_;
    result_data[r] = StringVector::EmptyString(result, bs);
    data_ptr[r] = reinterpret_cast<data_ptr_t>(result_data[r].GetDataWriteable());
  }
}

void CreateSortKeyInternal(std::vector<sort_key_data_ptr_t> &sort_key_data,
                           const std::vector<OrderModifiers> &modifiers, Vector &result, idx_t row_count) {
  // 1. Measure: how many bytes does each row's key need?
  SortKeyLengthInfo key_lengths(row_count);
  for (auto &vd : sort_key_data) {
    GetSortKeyLength(*vd, key_lengths);
  }

  // 2. Reserve: allocate the (still empty) keys in the result's heap.
  auto data_pointers = std::unique_ptr<data_ptr_t[]>(new data_ptr_t[row_count]);
  PrepareSortData(result, row_count, key_lengths, data_pointers.get());

  // 3. Fill: append each column's encoding to every row, in key order. `offsets` tracks how
  //    far into each row's key we have written, so the columns land back to back.
  std::vector<idx_t> offsets;
  offsets.resize(row_count, 0);
  for (idx_t c = 0; c < sort_key_data.size(); c++) {
    SortKeyConstructInfo info(modifiers[c], offsets, data_pointers.get());
    ConstructSortKey(*sort_key_data[c], info);
  }
}

}  // namespace

void CreateSortKey::Create(DataChunk &input, const std::vector<OrderModifiers> &modifiers, Vector &result) {
  BUMBLEBEE_ASSERT(modifiers.size() == input.ColumnCount(), "one order modifier per column");
  std::vector<sort_key_data_ptr_t> sort_key_data;
  sort_key_data.reserve(modifiers.size());
  for (idx_t r = 0; r < modifiers.size(); r++) {
    sort_key_data.push_back(std::make_unique<SortKeyVectorData>(input.data_[r], input.GetSize()));
  }
  CreateSortKeyInternal(sort_key_data, modifiers, result, input.GetSize());
}

void CreateSortKey::Create(Vector &input, idx_t size, const OrderModifiers &modifiers, Vector &result) {
  std::vector<sort_key_data_ptr_t> sort_key_data;
  sort_key_data.push_back(std::make_unique<SortKeyVectorData>(input, size));
  CreateSortKeyInternal(sort_key_data, {modifiers}, result, size);
}

}  // namespace bumblebee
