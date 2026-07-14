//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// sort_key_test.cpp
//
// Identification: test/unit/type/vector/operations/sort_key_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "null_test_base.h"
#include "type/vector/operations/create_sort_key.h"
#include "type/vector/vector.h"

namespace bumblebee {

class SortKeysTest : public BumbleBaseTest {
 protected:
  /**
   * @brief The core invariant: sorting the ENCODED keys with memcmp must reproduce exactly
   *        the order that sorting the VALUES with the SQL comparator produces.
   *
   * The value order comes from RowComparator, which knows nothing about the encoder — so
   * this is a real cross-check, not a tautology.
   */
  void CreateSortKeysAndCheckResult(std::vector<std::vector<Value>> &data, DataChunk &input,
                                    std::vector<OrderModifiers> &modifiers) {
    Vector result(PhysicalType::STRING);
    CreateSortKey::Create(input, modifiers, result);

    // Collect the encoded keys.
    std::vector<std::string> key_string;
    auto *data_ptr = FlatVector::GetData<string_t>(result);
    for (idx_t i = 0; i < input.GetSize(); ++i) {
      key_string.push_back(data_ptr[i].GetString());
    }

    // Turn the column-major test data into rows.
    std::vector<std::vector<Value>> table_data;
    for (idx_t row = 0; row < data[0].size(); ++row) {
      std::vector<Value> row_data;
      row_data.reserve(data.size());
      for (idx_t col = 0; col < data.size(); ++col) {
        row_data.push_back(data[col][row]);
      }
      table_data.push_back(std::move(row_data));
    }

    // Sort the ROWS by value, and record where each original row lands.
    std::vector<idx_t> idxs(table_data.size());
    std::iota(idxs.begin(), idxs.end(), 0);
    RowComparator cmp{modifiers, table_data};
    std::sort(idxs.begin(), idxs.end(), cmp);
    std::unordered_map<idx_t, idx_t> ordered_position;
    for (idx_t i = 0; i < idxs.size(); ++i) {
      ordered_position[idxs[i]] = i;
    }

    // Sort the KEYS as bytes, and check every key lands where its row did.
    std::unordered_map<std::string, idx_t> index_before_sort;
    for (idx_t i = 0; i < key_string.size(); i++) {
      if (!index_before_sort.contains(key_string[i])) {
        index_before_sort[key_string[i]] = i;
      }
    }
    std::sort(key_string.begin(), key_string.end());
    for (idx_t i = 0; i < key_string.size(); i++) {
      auto orig_idx = index_before_sort[key_string[i]];
      if (i != ordered_position[orig_idx] && key_string[i] == key_string[ordered_position[orig_idx]]) {
        // Two rows encoded identically, so their relative order is free.
        continue;
      }
      EXPECT_EQ(i, ordered_position[orig_idx]);
    }
  }
};

TEST_F(SortKeysTest, CreateSortKeysOneColAscTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<int>{0, 10, 20, 30});
  std::vector<LogicalType> types = {LogicalTypeId::UINTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysOneColAscIntTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<int>{0, 10, 20, 30});
  std::vector<LogicalType> types = {LogicalTypeId::BIGINT};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysOneColDescIntTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<int>{0, 10, 20, 30});
  std::vector<LogicalType> types = {LogicalTypeId::SMALLINT};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::DESCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysTwoColsDoubleDescIntAscTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<double>{1.5, -2.0, 3.1415, 0.0});
  AddData(data, std::vector<int>{10, 5, 20, 15});
  std::vector<LogicalType> types = {LogicalTypeId::DOUBLE, LogicalTypeId::INTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::DESCENDING, OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysThreeColsFloatUintStrMixedTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<float>{1.0F, 2.0F, 1.0F, 3.5F, 2.0F, 1.0F});
  AddData(data, std::vector<unsigned int>{2, 1, 2, 0, 1, 2});
  AddData(data, std::vector<std::string>{"b", "a", "a", "c", "b", "a"});
  std::vector<LogicalType> types = {LogicalTypeId::FLOAT, LogicalTypeId::UINTEGER, LogicalTypeId::STRING};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::ASCENDING, OrderType::DESCENDING, OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysFiveColsMixedTypesTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<int64_t>{10, 10, 5, 10, 5});
  AddData(data, std::vector<short>{1, 0, 2, 1, 2});
  AddData(data, std::vector<double>{0.1, -1.0, 0.1, 2.0, 0.1});
  AddData(data, std::vector<std::string>{"x", "x", "a", "y", "a"});
  AddData(data, std::vector<unsigned int>{1, 2, 1, 0, 1});
  std::vector<LogicalType> types = {LogicalTypeId::BIGINT, LogicalTypeId::SMALLINT, LogicalTypeId::DOUBLE,
                                    LogicalTypeId::STRING, LogicalTypeId::UINTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::ASCENDING, OrderType::DESCENDING, OrderType::ASCENDING,
                                           OrderType::DESCENDING, OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysTwoColsStringDescUintAscTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<std::string>{"apple", "banana", "apple", "zebra", "banana"});
  AddData(data, std::vector<unsigned int>{5, 3, 5, 1, 4});
  std::vector<LogicalType> types = {LogicalTypeId::STRING, LogicalTypeId::UINTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::DESCENDING, OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysThreeColsMixedOrderTypesTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<float>{0.0F, -0.0F, 1.5F, 1.5F, 2.0F, -1.0F, 1.5F});
  AddData(data, std::vector<double>{0.0, 0.0, 1.5, 1.4, 2.0, -1.0, 1.5});
  AddData(data, std::vector<int>{0, 1, 2, 1, 3, 0, 2});
  std::vector<LogicalType> types = {LogicalTypeId::FLOAT, LogicalTypeId::DOUBLE, LogicalTypeId::INTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::DESCENDING, OrderType::ASCENDING, OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysLongStringAscIntDescTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<std::string>{"this_is_a_long_string_1", "another_very_long_string_2",
                                         "zebra_long_string_3", "middle_length_string_4",
                                         "0_middle_length_string_4"});
  AddData(data, std::vector<int>{42, 7, 100, 55, 55});
  std::vector<LogicalType> types = {PhysicalType::STRING, PhysicalType::INTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::ASCENDING, OrderType::DESCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

TEST_F(SortKeysTest, CreateSortKeysLongStringDescUintAscTest) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<std::string>{"long_string_alpha_abcdefghij", "long_string_beta_klmnopqrst",
                                         "long_string_gamma_uvwxyzabc", "1_long_string_delta_defghijkl"});
  AddData(data, std::vector<unsigned int>{10, 5, 20, 15});
  std::vector<LogicalType> types = {LogicalTypeId::STRING, LogicalTypeId::UINTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::DESCENDING, OrderType::ASCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

// Values above INT32_MAX: the encoding must treat a UINTEGER as unsigned, not flip its
// sign bit the way it does for a signed type.
TEST_F(SortKeysTest, CreateSortKeysLongUInteger) {
  std::vector<std::vector<Value>> data;
  AddData(data, std::vector<uint32_t>{4293053207, 4293668640, 4294747978, 4279807690, 4293053207, 4286980034});
  std::vector<LogicalType> types = {LogicalTypeId::UINTEGER};
  DataChunk chunk = GenerateDataChunk(types, data);

  std::vector<OrderModifiers> modifiers = {OrderType::DESCENDING};
  CreateSortKeysAndCheckResult(data, chunk, modifiers);
}

// ---------------------------------------------------------------------------
// NULL semantics: NULLS LAST in ASC, NULLS FIRST in DESC.
// ---------------------------------------------------------------------------

class SortKeysNullTest : public NullTestBase {
 protected:
  /** @return The row indices in the order their encoded keys sort. */
  static auto SortedIndices(Vector &keys, idx_t size) -> std::vector<idx_t> {
    auto *data = FlatVector::GetData<string_t>(keys);
    std::vector<idx_t> idxs(size);
    std::iota(idxs.begin(), idxs.end(), 0);
    std::sort(idxs.begin(), idxs.end(),
              [&](idx_t a, idx_t b) { return data[a].GetString() < data[b].GetString(); });
    return idxs;
  }
};

// A single ASC int column with one NULL: the NULL sorts LAST.
TEST_F(SortKeysNullTest, AscNullsLast) {
  Vector v(PhysicalType::INTEGER, 4);
  v.SetValue(0, Value(30));
  v.SetValue(1, Value::Null(PhysicalType::INTEGER));
  v.SetValue(2, Value(10));
  v.SetValue(3, Value(20));

  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(v, 4, {OrderType::ASCENDING}, keys);
  auto order = SortedIndices(keys, 4);
  // [10, 20, 30, NULL] -> rows 2, 3, 0, 1
  EXPECT_EQ(order, (std::vector<idx_t>{2, 3, 0, 1}));
}

// The same column DESC: the NULL sorts FIRST — the natural consequence of flipping the
// same encoding, and the conventional SQL default.
TEST_F(SortKeysNullTest, DescNullsFirst) {
  Vector v(PhysicalType::INTEGER, 4);
  v.SetValue(0, Value(30));
  v.SetValue(1, Value::Null(PhysicalType::INTEGER));
  v.SetValue(2, Value(10));
  v.SetValue(3, Value(20));

  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(v, 4, {OrderType::DESCENDING}, keys);
  auto order = SortedIndices(keys, 4);
  // [NULL, 30, 20, 10] -> rows 1, 0, 3, 2
  EXPECT_EQ(order, (std::vector<idx_t>{1, 0, 3, 2}));
}

// A tie on the first column is broken by the second, where a NULL again sorts after a value.
TEST_F(SortKeysNullTest, TwoColAscSecondaryNullsLast) {
  DataChunk chunk;
  std::vector<LogicalType> types = {PhysicalType::INTEGER, PhysicalType::INTEGER};
  chunk.Initialize(types);
  chunk.SetCardinality(5);
  chunk.SetValue(0, 0, Value(1));
  chunk.SetValue(1, 0, Value::Null(PhysicalType::INTEGER));
  chunk.SetValue(0, 1, Value(1));
  chunk.SetValue(1, 1, Value(5));
  chunk.SetValue(0, 2, Value(1));
  chunk.SetValue(1, 2, Value(2));
  chunk.SetValue(0, 3, Value(2));
  chunk.SetValue(1, 3, Value::Null(PhysicalType::INTEGER));
  chunk.SetValue(0, 4, Value(2));
  chunk.SetValue(1, 4, Value(1));

  Vector keys(PhysicalType::STRING);
  std::vector<OrderModifiers> mods = {OrderType::ASCENDING, OrderType::ASCENDING};
  CreateSortKey::Create(chunk, mods, keys);
  auto order = SortedIndices(keys, 5);
  // (1,2), (1,5), (1,NULL), (2,1), (2,NULL) -> rows 2, 1, 0, 4, 3
  EXPECT_EQ(order, (std::vector<idx_t>{2, 1, 0, 4, 3}));
}

// Every value NULL: every key encodes to the same bytes.
TEST_F(SortKeysNullTest, AllNullsKeepsRelativeOrder) {
  Vector v(PhysicalType::BIGINT, 3);
  v.SetValue(0, Value::Null(PhysicalType::BIGINT));
  v.SetValue(1, Value::Null(PhysicalType::BIGINT));
  v.SetValue(2, Value::Null(PhysicalType::BIGINT));
  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(v, 3, {OrderType::ASCENDING}, keys);
  auto *data = FlatVector::GetData<string_t>(keys);
  // 3 rows of 8 bytes of 0xFF.
  auto k0 = data[0].GetString();
  EXPECT_EQ(data[1].GetString(), k0);
  EXPECT_EQ(data[2].GetString(), k0);
  EXPECT_EQ(k0.size(), sizeof(int64_t));
}

// STRING ASC with one NULL: every key opens with a marker byte, and a real string's marker
// sorts below a NULL's.
TEST_F(SortKeysNullTest, StringAscNullsLast) {
  Vector v(PhysicalType::STRING, 4);
  v.SetValue(0, Value("banana"));
  v.SetValue(1, Value::Null(PhysicalType::STRING));
  v.SetValue(2, Value("apple"));
  v.SetValue(3, Value("cherry"));

  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(v, 4, {OrderType::ASCENDING}, keys);
  auto order = SortedIndices(keys, 4);
  // apple, banana, cherry, NULL -> rows 2, 0, 3, 1
  EXPECT_EQ(order, (std::vector<idx_t>{2, 0, 3, 1}));
}

// STRING DESC mirrors the numeric case: the flip puts the NULLs first.
TEST_F(SortKeysNullTest, StringDescNullsFirst) {
  Vector v(PhysicalType::STRING, 4);
  v.SetValue(0, Value("banana"));
  v.SetValue(1, Value::Null(PhysicalType::STRING));
  v.SetValue(2, Value("apple"));
  v.SetValue(3, Value("cherry"));

  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(v, 4, {OrderType::DESCENDING}, keys);
  auto order = SortedIndices(keys, 4);
  // NULL, cherry, banana, apple -> rows 1, 3, 0, 2
  EXPECT_EQ(order, (std::vector<idx_t>{1, 3, 0, 2}));
}

// The byte-collision corner case: a string whose first byte is 0xFE, which the +1 shift
// turns into 0xFF. It must STILL sort below a NULL — the leading marker byte is what
// guarantees that, not the payload.
TEST_F(SortKeysNullTest, StringWithHighByteStillSortsBeforeNull) {
  Vector v(PhysicalType::STRING, 2);
  char buf[2] = {static_cast<char>(0xFE), 0};
  v.SetValue(0, Value(buf));
  v.SetValue(1, Value::Null(PhysicalType::STRING));

  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(v, 2, {OrderType::ASCENDING}, keys);
  auto order = SortedIndices(keys, 2);
  EXPECT_EQ(order, (std::vector<idx_t>{0, 1}));
}

// A dictionary input goes through the generic encode path: a NULL reached through the
// selection must still get the null marker.
TEST_F(SortKeysNullTest, DictionaryInputAscNullsLast) {
  Vector base(PhysicalType::INTEGER, 5);
  base.SetValue(0, Value(30));
  base.SetValue(1, Value(10));
  base.SetValue(2, Value::Null(PhysicalType::INTEGER));
  base.SetValue(3, Value(20));
  base.SetValue(4, Value(40));
  SelectionVector sel(4);
  sel.SetIndex(0, 0);  // 30
  sel.SetIndex(1, 1);  // 10
  sel.SetIndex(2, 2);  // NULL
  sel.SetIndex(3, 3);  // 20
  Vector dict(base, sel, 4);

  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(dict, 4, {OrderType::ASCENDING}, keys);
  auto order = SortedIndices(keys, 4);
  // 10, 20, 30, NULL -> the dictionary's rows 1, 3, 0, 2
  EXPECT_EQ(order, (std::vector<idx_t>{1, 3, 0, 2}));
}

// A NULL constant broadcast over N rows: every key must be identical.
TEST_F(SortKeysNullTest, ConstantNullVectorAllEqualKeys) {
  Value null_value = Value::Null(PhysicalType::INTEGER);
  Vector const_null(null_value);
  Vector keys(PhysicalType::STRING);
  CreateSortKey::Create(const_null, 5, {OrderType::ASCENDING}, keys);
  auto *data = FlatVector::GetData<string_t>(keys);
  for (idx_t i = 1; i < 5; i++) {
    EXPECT_EQ(data[i].GetString(), data[0].GetString());
  }
}

}  // namespace bumblebee
