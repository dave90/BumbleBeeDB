//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// generic_key.h
//
// Identification: src/include/storage/index/generic_key.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <cstring>

#include "catalog/schema.h"
#include "common/exception.h"
#include "common/helper.h"
#include "common/macros.h"

namespace bumblebee {

/**
 * @brief A fixed-size index key holding the raw bytes of the key columns.
 *
 * The bytes are laid out at the key schema's column offsets. `KeySize` must be large enough to hold
 * the key schema's inlined width; `SetFromKey` bounds-checks against it.
 */
template <size_t KeySize>
class GenericKey {
 public:
  /** @brief Copy `len` key bytes in, zero-padding the rest. Rejects an over-long key (bug #9). */
  void SetFromKey(const_data_ptr_t data, uint32_t len) {
    BUMBLEBEE_ASSERT(len <= KeySize, "GenericKey::SetFromKey: key longer than KeySize");
    std::memset(data_, 0, KeySize);
    std::memcpy(data_, data, len);
  }

  /** @brief Test helper: store a single integer key at offset 0. */
  void SetFromInteger(int64_t key) {
    std::memset(data_, 0, KeySize);
    std::memcpy(data_, &key, sizeof(int64_t));
  }

  /** @brief Test helper: read the first 8 bytes as an integer. */
  auto GetAsInteger() const -> int64_t {
    int64_t out;
    std::memcpy(&out, data_, sizeof(int64_t));
    return out;
  }

  data_t data_[KeySize];
};

/** @brief Compare two fixed-width integers, returning -1 / 0 / 1. */
template <class T>
inline auto CompareScalar(const_data_ptr_t a, const_data_ptr_t b, size_t offset) -> int {
  auto lhs = Load<T>(a + offset);
  auto rhs = Load<T>(b + offset);
  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

/**
 * @brief Orders two `GenericKey`s by comparing their columns, in schema order.
 *
 * Comparison is done directly on the key bytes with typed loads — no boxed `Value` on this hot path.
 * Only fixed-width key columns are supported; a variable-length key column throws.
 */
template <size_t KeySize>
class GenericComparator {
 public:
  explicit GenericComparator(const Schema *key_schema) : key_schema_(key_schema) {}
  GenericComparator(const GenericComparator &other) = default;

  auto operator()(const GenericKey<KeySize> &lhs, const GenericKey<KeySize> &rhs) const -> int {
    for (uint32_t i = 0; i < key_schema_->GetColumnCount(); i++) {
      const auto &col = key_schema_->GetColumn(i);
      auto off = col.GetOffset();
      int cmp = 0;
      switch (col.GetType().GetPhysicalType()) {
        case PhysicalType::TINYINT:
          cmp = CompareScalar<int8_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::SMALLINT:
          cmp = CompareScalar<int16_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::INTEGER:
          cmp = CompareScalar<int32_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::BIGINT:
          cmp = CompareScalar<int64_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::UTINYINT:
          cmp = CompareScalar<uint8_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::USMALLINT:
          cmp = CompareScalar<uint16_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::UINTEGER:
          cmp = CompareScalar<uint32_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::UBIGINT:
          cmp = CompareScalar<uint64_t>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::FLOAT:
          cmp = CompareScalar<float>(lhs.data_, rhs.data_, off);
          break;
        case PhysicalType::DOUBLE:
          cmp = CompareScalar<double>(lhs.data_, rhs.data_, off);
          break;
        default:
          throw NotImplementedException("GenericComparator: variable-length key columns are not supported");
      }
      if (cmp != 0) {
        return cmp;
      }
    }
    return 0;
  }

 private:
  const Schema *key_schema_;
};

}  // namespace bumblebee
