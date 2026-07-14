//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumble_base_test.h
//
// Identification: test/unit/include/bumble_base_test.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "common/exception.h"
#include "common/macros.h"
#include "gtest/gtest.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/operations/create_sort_key.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * The shared base of the vectorized-engine tests: a seeded RNG plus the value/data
 * generators the individual suites build their inputs from.
 */
class BumbleBaseTest : public ::testing::Test {
 protected:
  static const std::uint64_t SEED = 42;

  std::mt19937_64 rng_;

  BumbleBaseTest() : Test(), rng_(SEED) {}

  /**
   * @brief Orders two rows of a row-major table by the ORDER BY keys, as SQL would.
   *
   * This is the INDEPENDENT oracle the sort-key tests check the encoder against: sorting
   * the rows with this comparator has to agree with sorting the encoded keys with memcmp.
   * It deliberately does not use the encoder.
   */
  struct RowComparator {
    const std::vector<OrderModifiers> &modifiers_;
    const std::vector<std::vector<Value>> &data_;

    /** @return -1, 0 or 1 as `a` sorts below, with, or above `b`. NULLs sort LAST. */
    static auto CompareValues(const Value &a, const Value &b) -> int {
      // NULLS LAST, matching the encoder's 0xFF / prefix+1 markers.
      if (a.IsNull() || b.IsNull()) {
        if (a.IsNull() && b.IsNull()) {
          return 0;
        }
        return a.IsNull() ? 1 : -1;
      }
      if (a.GetPhysicalType() == PhysicalType::STRING) {
        const auto &sa = a.GetString();
        const auto &sb = b.GetString();
        if (sa == sb) {
          return 0;
        }
        return sa < sb ? -1 : 1;
      }
      // Every numeric physical type compares exactly through a long double: it carries the
      // full 64-bit integer range on the platforms this builds on, so no value is lost.
      auto da = a.GetAs<long double>();
      auto db = b.GetAs<long double>();
      if (da == db) {
        return 0;
      }
      return da < db ? -1 : 1;
    }

    auto operator()(std::size_t i, std::size_t j) const -> bool {
      const auto &a = data_[i];
      const auto &b = data_[j];

      idx_t col = 0;
      for (const auto &mod : modifiers_) {
        int cmp = CompareValues(a[col], b[col]);
        if (cmp == 0) {
          col++;
          continue;
        }
        switch (mod.order_type_) {
          case OrderType::ASCENDING:
            return cmp < 0;
          case OrderType::DESCENDING:
            return cmp > 0;
          default:
            col++;
        }
      }
      return false;
    }
  };

  /** @brief Render an integer as a decimal string with `scale` digits after the point. */
  template <class T>
  auto FormatDecimal(int scale, T value) -> std::string;

  /** @brief Render every element of `values` as a decimal string with `scale` digits. */
  template <class T>
  auto FormatDecimalVector(int scale, std::vector<T> &values) -> std::vector<std::string>;

  /** @brief Append `data` to `table` as one more column of Values. */
  template <class T>
  void AddData(std::vector<std::vector<Value>> &table, std::vector<T> data);

  /** @return The sequence [start, end], stepping by `step`. */
  auto GenerateSequence(int start, int end, int step = 1) -> std::vector<int>;

  /** @return `elements` values starting at `start`, stepping by `step`, wrapping past `end`. */
  auto GenerateSequence(int start, int end, int elements, int step) -> std::vector<int>;

  /** @return A random Value of the given integral type, drawn from the seeded RNG. */
  template <class T>
  auto GenerateRandomNumericValue() -> Value;

  /** @return A random Value of the given physical type, drawn from the seeded RNG. */
  auto RandomValue(PhysicalType type, idx_t max_string_len = 128) -> Value;

  // -- Vector / DataChunk generators ----------------------------------------

  /** @brief Write `values` into `v`, casting each to the vector's type. */
  template <class T>
  void SetValuesVector(Vector &v, std::vector<T> values);

  /** @return A FLAT Vector of `type` holding `values`, each cast to `type`. */
  template <class T>
  auto GenerateVector(LogicalType type, std::vector<T> values) -> Vector;

  /**
   * @brief A FLAT Vector of `type` whose rows hold `values` as the RAW physical payload.
   *
   * The difference from GenerateVector matters only for the types whose logical identity
   * differs from their storage: a DECIMAL(18,2) is a BIGINT holding a scaled integer, so
   * this writes 100 for 1.00. GenerateVector would try a logical BIGINT -> DECIMAL cast,
   * which is a different operation (and one Value does not do).
   */
  template <class T>
  auto GenerateRawVector(LogicalType type, std::vector<T> values) -> Vector;

  /** @return A FLAT Vector of `type` holding `values`, each cast to `type`. */
  virtual auto GenerateVector(LogicalType type, std::vector<Value> &values) -> Vector;

  /** @return A FLAT Vector of `size` rows holding 0, 1, ... cast to `type`. */
  virtual auto GenerateVector(idx_t size, LogicalType type) -> Vector;

  /** @return A DataChunk of `types` whose column `i` holds `data[i]`. */
  virtual auto GenerateDataChunk(std::vector<LogicalType> &types, std::vector<std::vector<Value>> &data) -> DataChunk;

  /** @return A DataChunk of STANDARD_VECTOR_SIZE random rows of `types`. */
  auto GenerateRandomDataChunk(std::vector<LogicalType> types) -> DataChunk;

  /**
   * @brief EXPECT that the two chunks hold the same rows, compared as sets of renderings.
   *
   * @param chunk1 The first chunk.
   * @param chunk2 The second chunk.
   * @param cols The columns to compare. Empty means every column.
   */
  virtual void CompareChunks(DataChunk &chunk1, DataChunk &chunk2, const std::vector<idx_t> &cols = {});
};

template <class T>
void BumbleBaseTest::SetValuesVector(Vector &v, std::vector<T> values) {
  for (idx_t i = 0; i < values.size(); i++) {
    Value val(values[i]);
    v.SetValue(i, val.CastAs(v.GetLogicalType()));
  }
}

template <class T>
auto BumbleBaseTest::GenerateVector(LogicalType type, std::vector<T> values) -> Vector {
  Vector v1(type, values.size());
  SetValuesVector<T>(v1, values);
  return v1;
}

template <class T>
auto BumbleBaseTest::GenerateRawVector(LogicalType type, std::vector<T> values) -> Vector {
  Vector v1(type, values.size());
  for (idx_t i = 0; i < values.size(); i++) {
    v1.SetValue(i, Value(values[i]).CastAs(type.GetPhysicalType()));
  }
  return v1;
}

inline auto BumbleBaseTest::GenerateVector(LogicalType type, std::vector<Value> &values) -> Vector {
  Vector v1(type, values.size());
  for (idx_t i = 0; i < values.size(); i++) {
    v1.SetValue(i, values[i].CastAs(v1.GetLogicalType()));
  }
  return v1;
}

inline auto BumbleBaseTest::GenerateVector(idx_t size, LogicalType type) -> Vector {
  Vector result(type);
  for (idx_t i = 0; i < size; i++) {
    result.SetValue(i, Value(static_cast<uint64_t>(i)).CastAs(type));
  }
  return result;
}

inline auto BumbleBaseTest::GenerateDataChunk(std::vector<LogicalType> &types, std::vector<std::vector<Value>> &data)
    -> DataChunk {
  BUMBLEBEE_ASSERT(types.size() == data.size(), "GenerateDataChunk: one column of data per type");
  DataChunk chunk;
  chunk.InitializeEmpty(types);
  idx_t idx = 0;
  for (auto &data_col : data) {
    Vector vec = GenerateVector(types[idx], data_col);
    // Reference shares the data manager, so the column outlives the local `vec`.
    chunk.data_[idx++].Reference(vec);
  }
  chunk.SetCapacity(data[0].size());
  chunk.SetCardinality(data[0].size());
  return chunk;
}

inline auto BumbleBaseTest::GenerateRandomDataChunk(std::vector<LogicalType> types) -> DataChunk {
  DataChunk chunk;
  chunk.Initialize(types);
  for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
    for (idx_t j = 0; j < types.size(); j++) {
      Value v = RandomValue(types[j].GetPhysicalType());
      chunk.SetValue(j, i, v);
    }
  }
  chunk.SetCardinality(STANDARD_VECTOR_SIZE);
  return chunk;
}

inline void BumbleBaseTest::CompareChunks(DataChunk &chunk1, DataChunk &chunk2, const std::vector<idx_t> &cols) {
  EXPECT_EQ(chunk1.ColumnCount(), chunk2.ColumnCount());
  EXPECT_EQ(chunk1.GetSize(), chunk2.GetSize());

  // An empty `cols` means every column. NOTE: the original resized nothing before the
  // std::iota, so the set stayed empty and every column was skipped — the default-argument
  // call compared nothing at all. Fill the range properly instead.
  std::vector<idx_t> internal_cols = cols;
  if (internal_cols.empty()) {
    internal_cols.resize(chunk1.ColumnCount());
    std::iota(internal_cols.begin(), internal_cols.end(), 0);
  }
  std::unordered_set<idx_t> cols_set(internal_cols.begin(), internal_cols.end());

  std::unordered_set<std::string> chunk1_str;
  std::unordered_set<std::string> chunk2_str;
  for (idx_t i = 0; i < chunk1.GetSize(); i++) {
    std::string row1;
    std::string row2;
    for (idx_t j = 0; j < chunk1.ColumnCount(); j++) {
      if (!cols_set.contains(j)) {
        continue;
      }
      row1 += chunk1.GetValue(j, i).ToString() + " ; ";
      row2 += chunk2.GetValue(j, i).ToString() + " ; ";
    }
    chunk1_str.insert(row1);
    chunk2_str.insert(row2);
  }
  EXPECT_EQ(chunk1_str.size(), chunk2_str.size());
  EXPECT_EQ(chunk1_str, chunk2_str);
}

template <class T>
auto BumbleBaseTest::FormatDecimal(int scale, T value) -> std::string {
  if (scale <= 0) {
    return std::to_string(value);
  }

  std::string s = std::to_string(value);

  // Handle negative numbers.
  bool negative = (s[0] == '-');
  if (negative) {
    s.erase(s.begin());
  }

  // Ensure there are enough digits for the decimal point.
  if (static_cast<int>(s.size()) <= scale) {
    s.insert(0, scale - s.size() + 1, '0');
  }

  // Insert the decimal point.
  s.insert(s.end() - scale, '.');

  if (negative) {
    s.insert(s.begin(), '-');
  }

  return s;
}

template <class T>
auto BumbleBaseTest::FormatDecimalVector(int scale, std::vector<T> &values) -> std::vector<std::string> {
  std::vector<std::string> result;
  result.reserve(values.size());
  for (auto &v : values) {
    result.push_back(FormatDecimal(scale, v));
  }
  return result;
}

template <class T>
void BumbleBaseTest::AddData(std::vector<std::vector<Value>> &table, std::vector<T> data) {
  std::vector<Value> col_data;
  for (auto &d : data) {
    col_data.emplace_back(d);
  }
  table.push_back(std::move(col_data));
}

template <class T>
auto BumbleBaseTest::GenerateRandomNumericValue() -> Value {
  std::uniform_int_distribution<T> dist(std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
  return Value(dist(rng_));
}

inline auto BumbleBaseTest::GenerateSequence(int start, int end, int step) -> std::vector<int> {
  std::vector<int> result;
  for (int i = start; i <= end; i += step) {
    result.push_back(i);
  }
  return result;
}

inline auto BumbleBaseTest::GenerateSequence(int start, int end, int elements, int step) -> std::vector<int> {
  std::vector<int> result;
  auto t = start;
  for (int i = 0; i < elements; i++) {
    result.push_back(t);
    t += step;
    if (t > end) {
      t = start;
    }
  }
  return result;
}

inline auto BumbleBaseTest::RandomValue(PhysicalType type, idx_t max_string_len) -> Value {
  switch (type) {
    case PhysicalType::TINYINT:
      return GenerateRandomNumericValue<int8_t>();
    case PhysicalType::SMALLINT:
      return GenerateRandomNumericValue<int16_t>();
    case PhysicalType::INTEGER:
      return GenerateRandomNumericValue<int32_t>();
    case PhysicalType::BIGINT:
      return GenerateRandomNumericValue<int64_t>();
    case PhysicalType::UTINYINT:
      return GenerateRandomNumericValue<uint8_t>();
    case PhysicalType::USMALLINT:
      return GenerateRandomNumericValue<uint16_t>();
    case PhysicalType::UINTEGER:
      return GenerateRandomNumericValue<uint32_t>();
    case PhysicalType::UBIGINT:
      return GenerateRandomNumericValue<uint64_t>();
    case PhysicalType::FLOAT: {
      // Keep the float range reasonable to avoid infinities.
      std::uniform_real_distribution<float> dist(-1.0e6F, 1.0e6F);
      return Value(dist(rng_));
    }
    case PhysicalType::DOUBLE: {
      std::uniform_real_distribution<double> dist(-1.0e12, 1.0e12);
      return Value(dist(rng_));
    }
    case PhysicalType::STRING: {
      // Random length in [0, max_string_len]. An empty string is a valid outcome.
      if (max_string_len == 0) {
        return Value(std::string{});
      }

      std::uniform_int_distribution<idx_t> len_dist(0, max_string_len);
      idx_t len = len_dist(rng_);

      static const char CHARSET[] =
          "0123456789"
          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "abcdefghijklmnopqrstuvwxyz";
      static constexpr std::size_t CHARSET_SIZE = sizeof(CHARSET) - 1;  // exclude '\0'

      std::uniform_int_distribution<std::size_t> char_dist(0, CHARSET_SIZE - 1);

      std::string s;
      s.reserve(len);
      for (idx_t i = 0; i < len; ++i) {
        s.push_back(CHARSET[char_dist(rng_)]);
      }
      return Value(std::move(s));
    }
    default:
      throw NotImplementedException("RandomValue: unsupported PhysicalType");
  }
}

}  // namespace bumblebee
