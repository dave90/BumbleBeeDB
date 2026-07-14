//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// type_test.cpp
//
// Identification: test/unit/type/type_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "type/logical_type.h"
#include "type/value.h"

namespace bumblebee {

// ---------------------------------------------------------------------------
// LogicalType
// ---------------------------------------------------------------------------

TEST(LogicalTypeTest, ScalarPhysicalMapping) {
  EXPECT_EQ(LogicalType(LogicalTypeId::INTEGER).GetPhysicalType(), PhysicalType::INTEGER);
  EXPECT_EQ(LogicalType(LogicalTypeId::BIGINT).GetPhysicalType(), PhysicalType::BIGINT);
  // BOOLEAN, DATE and TIMESTAMP have no physical type of their own.
  EXPECT_EQ(LogicalType(LogicalTypeId::BOOLEAN).GetPhysicalType(), PhysicalType::UTINYINT);
  EXPECT_EQ(LogicalType(LogicalTypeId::DATE).GetPhysicalType(), PhysicalType::INTEGER);
  EXPECT_EQ(LogicalType(LogicalTypeId::TIMESTAMP).GetPhysicalType(), PhysicalType::BIGINT);
}

TEST(LogicalTypeTest, BooleanIsDistinctFromItsPhysicalType) {
  // The whole reason Value carries a LogicalType: these two share a physical
  // representation but must not compare equal.
  EXPECT_NE(LogicalType(LogicalTypeId::BOOLEAN), LogicalType(LogicalTypeId::UTINYINT));
}

TEST(LogicalTypeTest, ToStringScalar) {
  EXPECT_EQ(LogicalType(LogicalTypeId::INTEGER).ToString(), "INTEGER");
  EXPECT_EQ(LogicalType(LogicalTypeId::STRING).ToString(), "VARCHAR");
  EXPECT_EQ(LogicalType(LogicalTypeId::BOOLEAN).ToString(), "BOOLEAN");
}

TEST(LogicalTypeTest, Decimal) {
  auto d = LogicalType::Decimal(10, 2);
  EXPECT_EQ(d.GetTypeId(), LogicalTypeId::DECIMAL);
  EXPECT_EQ(d.GetDecimalData().width_, 10);
  EXPECT_EQ(d.GetDecimalData().scale_, 2);
  EXPECT_EQ(d.ToString(), "DECIMAL(10,2)");
  // The backing width narrows with precision.
  EXPECT_EQ(LogicalType::Decimal(4, 1).GetPhysicalType(), PhysicalType::SMALLINT);
  EXPECT_EQ(LogicalType::Decimal(9, 1).GetPhysicalType(), PhysicalType::INTEGER);
  EXPECT_EQ(LogicalType::Decimal(18, 1).GetPhysicalType(), PhysicalType::BIGINT);
  EXPECT_NE(LogicalType::Decimal(10, 2), LogicalType::Decimal(10, 3));
}

TEST(LogicalTypeTest, DecimalTooWideThrows) {
  EXPECT_THROW(LogicalType::Decimal(30, 2), NotImplementedException);
}

// ---------------------------------------------------------------------------
// ARRAY / LIST — the DuckDB-style addition
// ---------------------------------------------------------------------------

TEST(LogicalTypeTest, ListRoundTrips) {
  auto list = LogicalType::List(LogicalTypeId::INTEGER);
  EXPECT_EQ(list.GetTypeId(), LogicalTypeId::LIST);
  EXPECT_EQ(list.GetPhysicalType(), PhysicalType::LIST);
  EXPECT_EQ(list.GetChildType(), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(list.ToString(), "INTEGER[]");
}

TEST(LogicalTypeTest, ArrayRoundTrips) {
  auto array = LogicalType::Array(LogicalTypeId::INTEGER, 3);
  EXPECT_EQ(array.GetTypeId(), LogicalTypeId::ARRAY);
  EXPECT_EQ(array.GetPhysicalType(), PhysicalType::ARRAY);
  EXPECT_EQ(array.GetListData().size_, 3U);
  EXPECT_EQ(array.GetChildType(), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(array.ToString(), "INTEGER[3]");
}

TEST(LogicalTypeTest, ListIsNotArray) {
  EXPECT_NE(LogicalType::List(LogicalTypeId::INTEGER),
            LogicalType::Array(LogicalTypeId::INTEGER, 3));
}

TEST(LogicalTypeTest, ArraysDifferBySizeAndChildType) {
  EXPECT_NE(LogicalType::Array(LogicalTypeId::INTEGER, 3),
            LogicalType::Array(LogicalTypeId::INTEGER, 4));
  EXPECT_NE(LogicalType::Array(LogicalTypeId::INTEGER, 3),
            LogicalType::Array(LogicalTypeId::BIGINT, 3));
  EXPECT_EQ(LogicalType::Array(LogicalTypeId::INTEGER, 3),
            LogicalType::Array(LogicalTypeId::INTEGER, 3));
}

TEST(LogicalTypeTest, NestedList) {
  auto nested = LogicalType::List(LogicalType::List(LogicalTypeId::INTEGER));
  EXPECT_EQ(nested.ToString(), "INTEGER[][]");
}

TEST(LogicalTypeTest, FromString) {
  EXPECT_EQ(LogicalType::FromString("int4"), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(LogicalType::FromString("INTEGER"), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(LogicalType::FromString("varchar"), LogicalType(LogicalTypeId::STRING));
  EXPECT_EQ(LogicalType::FromString("bool"), LogicalType(LogicalTypeId::BOOLEAN));
  EXPECT_EQ(LogicalType::FromString("nonsense"), LogicalType(LogicalTypeId::UNKNOWN));
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

TEST(ValueTest, NumericConstruction) {
  Value i{static_cast<int32_t>(42)};
  EXPECT_FALSE(i.IsNull());
  EXPECT_EQ(i.GetType(), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(i.GetAs<int32_t>(), 42);
  EXPECT_EQ(i.GetAs<int64_t>(), 42);
  EXPECT_EQ(i.ToString(), "42");
}

TEST(ValueTest, BooleanIsBoolean) {
  Value t{true};
  EXPECT_EQ(t.GetType(), LogicalType(LogicalTypeId::BOOLEAN));
  EXPECT_EQ(t.ToString(), "true");
  EXPECT_EQ(Value{false}.ToString(), "false");
  // A boolean must not silently equal the integer 1.
  EXPECT_NE(t, Value{static_cast<uint8_t>(1)});
}

TEST(ValueTest, StringValue) {
  Value s{"hello"};
  EXPECT_EQ(s.GetType(), LogicalType(LogicalTypeId::STRING));
  EXPECT_EQ(s.GetString(), "hello");
  EXPECT_EQ(s.ToString(), "'hello'");
  EXPECT_EQ(s, Value{"hello"});
  EXPECT_NE(s, Value{"world"});
}

TEST(ValueTest, LongStringValue) {
  // Longer than any small-string optimization threshold — just checking the
  // plain std::string payload survives copies.
  const std::string long_str = "the quick brown fox jumps over the lazy dog";
  Value s{long_str};
  Value copy = s;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(copy.GetString(), long_str);
  EXPECT_EQ(copy, s);
}

TEST(ValueTest, ReadingAStringAsANumberThrows) {
  EXPECT_THROW(Value{"hello"}.GetAs<int32_t>(), Exception);
  EXPECT_THROW(Value{static_cast<int32_t>(1)}.GetString(), Exception);
}

TEST(ValueTest, Null) {
  auto n = Value::Null(LogicalTypeId::INTEGER);
  EXPECT_TRUE(n.IsNull());
  EXPECT_EQ(n.GetType(), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(n.ToString(), "NULL");
  // A NULL still knows its type.
  EXPECT_NE(n, Value::Null(LogicalTypeId::BIGINT));
  EXPECT_EQ(n, Value::Null(LogicalTypeId::INTEGER));
  EXPECT_NE(n, Value{static_cast<int32_t>(0)});
}

TEST(ValueTest, DefaultConstructedIsNull) {
  Value v;
  EXPECT_TRUE(v.IsNull());
  EXPECT_EQ(v.GetType(), LogicalType(LogicalTypeId::UNKNOWN));
}

TEST(ValueTest, CastAs) {
  Value i{static_cast<int32_t>(1)};
  EXPECT_EQ(i.CastAs(LogicalTypeId::BIGINT).GetType(), LogicalType(LogicalTypeId::BIGINT));
  EXPECT_EQ(i.CastAs(LogicalTypeId::BIGINT).GetAs<int64_t>(), 1);
  // This is the exact path the optimizer's IsPredicateTrue rule takes.
  EXPECT_TRUE(i.CastAs(LogicalTypeId::BOOLEAN).GetAs<int8_t>() != 0);
  EXPECT_FALSE(Value{static_cast<int32_t>(0)}.CastAs(LogicalTypeId::BOOLEAN).GetAs<int8_t>() != 0);
  EXPECT_EQ(i.CastAs(LogicalTypeId::DOUBLE).GetAs<double>(), 1.0);
}

TEST(ValueTest, CastNullStaysNull) {
  auto n = Value::Null(LogicalTypeId::INTEGER);
  auto cast = n.CastAs(LogicalTypeId::BIGINT);
  EXPECT_TRUE(cast.IsNull());
  EXPECT_EQ(cast.GetType(), LogicalType(LogicalTypeId::BIGINT));
}

TEST(ValueTest, ArrayValueHoldsItsChildren) {
  auto type = LogicalType::Array(LogicalTypeId::INTEGER, 3);
  auto v = Value::List(type, {Value{static_cast<int32_t>(1)}, Value{static_cast<int32_t>(2)},
                              Value{static_cast<int32_t>(3)}});
  EXPECT_FALSE(v.IsNull());
  EXPECT_EQ(v.GetType(), type);
  EXPECT_EQ(v.GetChildren().size(), 3U);
  EXPECT_EQ(v.GetChildren()[1], Value{static_cast<int32_t>(2)});
  EXPECT_EQ(v.ToString(), "[1, 2, 3]");
}

TEST(ValueTest, ListValueEquality) {
  auto type = LogicalType::List(LogicalTypeId::INTEGER);
  auto a = Value::List(type, {Value{static_cast<int32_t>(1)}, Value{static_cast<int32_t>(2)}});
  auto b = Value::List(type, {Value{static_cast<int32_t>(1)}, Value{static_cast<int32_t>(2)}});
  auto c = Value::List(type, {Value{static_cast<int32_t>(1)}});
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(ValueTest, ScalarHasNoChildren) {
  EXPECT_TRUE(Value{static_cast<int32_t>(1)}.GetChildren().empty());
}

}  // namespace bumblebee
