//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// row_operations_test.cpp
//
// Identification: test/unit/storage/row/row_operations_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/row/row_operations.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "gtest/gtest.h"
#include "storage/row/row_layout.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

namespace {

struct RowSpec {
  int32_t a_;
  std::string s_;
  std::optional<int32_t> c_;  // nullable
  double d_;
};

}  // namespace

TEST(RowLayoutTest, FixedWidthAndOffsets) {
  RowLayout layout;
  layout.Initialize({LogicalTypeId::INTEGER, LogicalTypeId::BIGINT});
  // 2 columns -> 1 validity byte, then INTEGER(4) at 1, BIGINT(8) at 5, aligned to 16.
  EXPECT_EQ(layout.GetFlagWidth(), 1U);
  EXPECT_EQ(layout.GetOffsets()[0], 1U);
  EXPECT_EQ(layout.GetOffsets()[1], 5U);
  EXPECT_TRUE(layout.AllConstant());
}

// The core bridge: scatter a DataChunk into row slots via RowOperations, gather it back, and assert
// the DataChunk round-trips cell-for-cell — including NULLs (validity prefix, bug #10) and both a
// short inlined string and a long payload string stored in the slot.
TEST(RowOperationsTest, ScatterGatherRoundTrip) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::STRING, LogicalTypeId::INTEGER,
                                 LogicalTypeId::DOUBLE};
  std::vector<RowSpec> spec{
      {10, "hi", 100, 1.5},
      {20, "a considerably longer string exceeding inline capacity", std::nullopt, 2.5},
      {30, "", 300, 3.5},
  };
  const idx_t count = spec.size();

  DataChunk in;
  in.Initialize(types);
  for (idx_t i = 0; i < count; i++) {
    in.SetValue(0, i, Value(spec[i].a_));
    in.SetValue(1, i, Value(spec[i].s_));
    in.SetValue(2, i, spec[i].c_.has_value() ? Value(*spec[i].c_) : Value::Null(LogicalTypeId::INTEGER));
    in.SetValue(3, i, Value(spec[i].d_));
  }
  in.SetCardinality(count);

  RowLayout layout;
  layout.Initialize(types);

  // Allocate a slot per row, sized fixed + this row's varlen payload (the one STRING column).
  std::vector<std::vector<char>> storage(count);
  for (idx_t i = 0; i < count; i++) {
    storage[i].assign(layout.GetFixedRowWidth() + spec[i].s_.size(), 0);
  }
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < count; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(storage[i].data());
  }

  SelectionVector identity;  // null selection -> GetIndex(i) == i
  RowOperations::Scatter(in, layout, rows, identity, count);

  DataChunk out;
  out.Initialize(types);
  for (idx_t col = 0; col < types.size(); col++) {
    RowOperations::FullScanColumn(layout, rows, out.data_[col], count, col);
  }
  out.SetCardinality(count);

  for (idx_t i = 0; i < count; i++) {
    for (idx_t j = 0; j < types.size(); j++) {
      EXPECT_EQ(out.GetValue(j, i), in.GetValue(j, i)) << "mismatch at row " << i << " col " << j;
    }
  }
  // The nullable column's NULL survived the round-trip (validity prefix).
  EXPECT_TRUE(out.GetValue(2, 1).IsNull());
  EXPECT_FALSE(out.GetValue(2, 0).IsNull());
  // The long string survived as a slot payload.
  EXPECT_EQ(out.GetValue(1, 1).GetString(), spec[1].s_);
}

// Scatter must read its input THROUGH each column's encoding, not assume flat: a DICTIONARY-encoded
// column (what a sliced chunk carries) has to be flattened on the way into the row bytes, or the raw
// selection indices would be written instead of the values.
TEST(RowOperationsTest, ScatterFlattensDictionaryColumn) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::STRING};
  const char *names[6] = {"zero", "one", "two", "three", "four", "five"};

  DataChunk in;
  in.Initialize(types);
  for (idx_t i = 0; i < 6; i++) {
    in.SetValue(0, i, Value(static_cast<int32_t>(i * 10)));
    in.SetValue(1, i, Value(std::string(names[i])));
  }
  in.SetCardinality(6);

  // Slicing to a reordered subset turns every column into a DICTIONARY over the original data.
  SelectionVector sel(3);
  sel.SetIndex(0, 4);  // 40 / "four"
  sel.SetIndex(1, 1);  // 10 / "one"
  sel.SetIndex(2, 5);  // 50 / "five"
  in.Slice(sel, 3);
  ASSERT_EQ(in.data_[0].GetVectorType(), VectorType::DICTIONARY_VECTOR);
  const idx_t count = 3;

  RowLayout layout;
  layout.Initialize(types);
  std::vector<std::vector<char>> storage(count);
  for (idx_t i = 0; i < count; i++) {
    storage[i].assign(layout.GetFixedRowWidth() + in.GetValue(1, i).GetString().size(), 0);
  }
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < count; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(storage[i].data());
  }

  SelectionVector identity;
  RowOperations::Scatter(in, layout, rows, identity, count);

  DataChunk out;
  out.Initialize(types);
  for (idx_t c = 0; c < types.size(); c++) {
    RowOperations::FullScanColumn(layout, rows, out.data_[c], count, c);
  }
  out.SetCardinality(count);

  for (idx_t i = 0; i < count; i++) {
    EXPECT_EQ(out.GetValue(0, i), in.GetValue(0, i)) << "row " << i;
    EXPECT_EQ(out.GetValue(1, i).GetString(), in.GetValue(1, i).GetString()) << "row " << i;
  }
  EXPECT_EQ(out.GetValue(0, 0), Value(40));
  EXPECT_EQ(out.GetValue(1, 0).GetString(), "four");
}

// A column that is entirely NULL must round-trip with every validity bit cleared.
TEST(RowOperationsTest, ScatterAllNullColumnRoundTrips) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::INTEGER};
  const idx_t count = 4;

  DataChunk in;
  in.Initialize(types);
  for (idx_t i = 0; i < count; i++) {
    in.SetValue(0, i, Value(static_cast<int32_t>(i + 1)));
    in.SetValue(1, i, Value::Null(LogicalTypeId::INTEGER));  // column 1 is all NULL
  }
  in.SetCardinality(count);

  RowLayout layout;
  layout.Initialize(types);
  std::vector<std::vector<char>> storage(count);
  for (idx_t i = 0; i < count; i++) {
    storage[i].assign(layout.GetFixedRowWidth(), 0);
  }
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < count; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(storage[i].data());
  }

  SelectionVector identity;
  RowOperations::Scatter(in, layout, rows, identity, count);

  DataChunk out;
  out.Initialize(types);
  for (idx_t c = 0; c < types.size(); c++) {
    RowOperations::FullScanColumn(layout, rows, out.data_[c], count, c);
  }
  out.SetCardinality(count);

  for (idx_t i = 0; i < count; i++) {
    EXPECT_FALSE(out.GetValue(0, i).IsNull());
    EXPECT_EQ(out.GetValue(0, i), Value(static_cast<int32_t>(i + 1)));
    EXPECT_TRUE(out.GetValue(1, i).IsNull()) << "row " << i;
  }
}

// The vectorized index key gather: build packed fixed-size keys from a subset of chunk columns, in a
// different order, with mixed widths — exactly what CreateIndex does — and verify each key byte-for-
// byte against the source, with no Value boxing.
TEST(RowOperationsTest, ScatterKeysGathersFixedWidthColumnsReordered) {
  std::vector<LogicalType> types{LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::BIGINT),
                                 LogicalType(LogicalTypeId::SMALLINT), LogicalType(LogicalTypeId::DOUBLE)};
  const idx_t count = 3;
  const std::vector<int32_t> a{10, 20, 30};
  const std::vector<int64_t> b{1000, 2000, 3000};
  const std::vector<int16_t> c{-1, -2, -3};
  const std::vector<double> d{1.5, 2.5, 3.5};

  DataChunk in;
  in.Initialize(types);
  for (idx_t i = 0; i < count; i++) {
    in.SetValue(0, i, Value(a[i]));
    in.SetValue(1, i, Value(b[i]));
    in.SetValue(2, i, Value(c[i]));
    in.SetValue(3, i, Value(d[i]));
  }
  in.SetCardinality(count);

  // Key = (DOUBLE col 3, INTEGER col 0, SMALLINT col 2) — a reordered subset of the table columns.
  std::vector<uint32_t> src_cols{3, 0, 2};
  Schema key_schema{std::vector<Column>{
      Column("kd", LogicalType(LogicalTypeId::DOUBLE)),
      Column("ka", LogicalType(LogicalTypeId::INTEGER)),
      Column("kc", LogicalType(LogicalTypeId::SMALLINT)),
  }};
  std::vector<idx_t> dst_offsets;
  std::vector<PhysicalType> key_types;
  for (uint32_t k = 0; k < src_cols.size(); k++) {
    dst_offsets.push_back(key_schema.GetColumn(k).GetOffset());
    key_types.push_back(key_schema.GetColumn(k).GetType().GetPhysicalType());
  }
  const size_t stride = key_schema.GetInlinedStorageSize();

  std::vector<data_t> keys(count * stride, 0);
  RowOperations::ScatterKeys(in, src_cols, dst_offsets, key_types, keys.data(), stride, count);

  auto decode = [&](const_data_ptr_t key, idx_t off, auto tag) {
    decltype(tag) v;
    std::memcpy(&v, key + off, sizeof(v));
    return v;
  };
  for (idx_t i = 0; i < count; i++) {
    const_data_ptr_t key = keys.data() + i * stride;
    EXPECT_EQ(decode(key, dst_offsets[0], double{}), d[i]);
    EXPECT_EQ(decode(key, dst_offsets[1], int32_t{}), a[i]);
    EXPECT_EQ(decode(key, dst_offsets[2], int16_t{}), c[i]);
  }
}

// A variable-length key column is rejected (matches GenericComparator's fixed-width-only support).
TEST(RowOperationsTest, ScatterKeysRejectsVariableLengthColumn) {
  std::vector<LogicalType> types{LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::STRING)};
  DataChunk in;
  in.Initialize(types);
  in.SetValue(0, 0, Value(1));
  in.SetValue(1, 0, Value(std::string("nope")));
  in.SetCardinality(1);

  std::vector<uint32_t> src_cols{1};
  std::vector<idx_t> dst_offsets{0};
  std::vector<PhysicalType> key_types{PhysicalType::STRING};
  std::vector<data_t> keys(64, 0);
  EXPECT_THROW(RowOperations::ScatterKeys(in, src_cols, dst_offsets, key_types, keys.data(), 64, 1),
               NotImplementedException);
}

}  // namespace bumblebee
