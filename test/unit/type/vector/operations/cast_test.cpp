//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// cast_test.cpp
//
// Identification: test/unit/type/vector/operations/cast_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "bumble_base_test.h"
#include "common/numeric_utils.h"
#include "gtest/gtest.h"
#include "type/decimal.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

class VectorOperationsCastTest : public BumbleBaseTest {};

TEST_F(VectorOperationsCastTest, BasicCast) {
  std::vector<int> data = {0, 10, 20, 30};
  PhysicalType source_type = PhysicalType::INTEGER;
  PhysicalType result_type = PhysicalType::BIGINT;
  Vector input = GenerateVector(source_type, data);
  EXPECT_EQ(input.GetType(), source_type);
  Vector result(result_type, data.size());

  VectorOperations::Cast(input, result, data.size());
  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result.GetValue(i).GetAs<int64_t>(), static_cast<int64_t>(data[i]));
  }
}

TEST_F(VectorOperationsCastTest, UintToIntCast) {
  std::vector<int> data = {0, 10, 20, 30};
  PhysicalType source_type = PhysicalType::USMALLINT;
  PhysicalType result_type = PhysicalType::SMALLINT;
  Vector input = GenerateVector(source_type, data);
  EXPECT_EQ(input.GetType(), source_type);
  Vector result(result_type, data.size());

  VectorOperations::Cast(input, result, data.size());
  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result.GetValue(i).GetAs<int64_t>(), static_cast<int64_t>(data[i]));
  }
}

TEST_F(VectorOperationsCastTest, IntToFloatCast) {
  std::vector<int> data = {0, 10, 20, 30};
  PhysicalType source_type = PhysicalType::USMALLINT;
  PhysicalType result_type = PhysicalType::FLOAT;
  Vector input = GenerateVector(source_type, data);
  EXPECT_EQ(input.GetType(), source_type);
  Vector result(result_type, data.size());

  VectorOperations::Cast(input, result, data.size());
  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result.GetValue(i).GetAs<double>(), static_cast<double>(data[i]));
  }
}

TEST_F(VectorOperationsCastTest, TryCastBigIntIntoSmall) {
  std::vector<uint64_t> data = {0, 10, 20, 30};
  PhysicalType source_type = PhysicalType::UBIGINT;
  PhysicalType result_type = PhysicalType::USMALLINT;
  Vector input1 = GenerateVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type);
  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result1.GetValue(i).GetAs<uint16_t>(), static_cast<uint16_t>(data[i]));
  }

  // 65536 does not fit in a USMALLINT: the cast must report the failure, not wrap.
  data.push_back(65536);
  Vector input2 = GenerateVector(source_type, data);
  Vector result2(result_type, data.size());
  r = VectorOperations::TryCast(input2, result2, data.size(), error.get());
  EXPECT_EQ(r, false);
  EXPECT_GE(error->size(), 0);
}

TEST_F(VectorOperationsCastTest, TryCastBigIntIntoString) {
  std::vector<uint64_t> data = {0, 10, 20, 30, 100000};
  PhysicalType source_type = PhysicalType::UBIGINT;
  PhysicalType result_type = PhysicalType::STRING;
  Vector input1 = GenerateVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type);
  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result1.GetValue(i).GetString(), std::to_string(data[i]));
  }
}

TEST_F(VectorOperationsCastTest, TryCastStringIntoBigInt) {
  std::vector<std::string> data = {"0", "10", "20", "30", "100002340"};
  PhysicalType source_type = PhysicalType::STRING;
  PhysicalType result_type = PhysicalType::BIGINT;
  Vector input1 = GenerateVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type);
  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result1.GetValue(i).GetAs<int64_t>(), atoi(data[i].c_str()));
  }

  // "12a3" is not a number: the cast must report the failure.
  data.emplace_back("12a3");
  Vector input2 = GenerateVector(source_type, data);
  Vector result2(result_type, data.size());
  error = std::make_unique<std::string>();
  r = VectorOperations::TryCast(input2, result2, data.size(), error.get());
  EXPECT_EQ(r, false);
  EXPECT_GE(error->size(), 0);
}

TEST_F(VectorOperationsCastTest, TryCastDecimalIntoString) {
  std::vector<uint64_t> data = {2, 11, 20, 30, 100000, 1};
  auto scale = 2;
  LogicalType source_type = LogicalType::Decimal(9, scale);
  PhysicalType result_type = PhysicalType::STRING;
  Vector input1 = GenerateRawVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type.GetPhysicalType());
  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  auto expected = FormatDecimalVector(scale, data);
  for (idx_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(result1.GetValue(i).GetString(), expected[i]);
  }
}

TEST_F(VectorOperationsCastTest, TryCastIntegerIntoDecimal) {
  std::vector<uint64_t> data = {2, 11, 20, 30, 100000, 1};
  auto scale = 2;
  LogicalType source_type = LogicalTypeId::INTEGER;
  LogicalType result_type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, scale);
  ASSERT_EQ(result_type.GetPhysicalType(), PhysicalType::BIGINT);
  Vector input1 = GenerateVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type.GetPhysicalType());

  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  for (idx_t i = 0; i < data.size(); i++) {
    // The integer N becomes the DECIMAL N.00, i.e. the backing integer N * 10^scale.
    auto scaled_data = result1.GetValue(i).GetAs<int64_t>() / NumericHelper::POWERS_OF_TEN[scale];
    EXPECT_EQ(scaled_data, data[i]);
  }
}

TEST_F(VectorOperationsCastTest, TryCastDoubleIntoDecimal) {
  std::vector<double> data = {2, 11.04, 20.009, 30.123, 100000, 1.99999};
  std::vector<std::string> expected_data = {"2.00", "11.04", "20.01", "30.12", "100000.00", "2.00"};
  auto scale = 2;
  LogicalType source_type = LogicalTypeId::DOUBLE;
  LogicalType result_type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, scale);
  ASSERT_EQ(result_type.GetPhysicalType(), PhysicalType::BIGINT);
  Vector input1 = GenerateVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type.GetPhysicalType());

  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  Vector result2(LogicalTypeId::STRING, data.size());
  VectorOperations::TryCast(result1, result2, data.size(), error.get());

  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result2.GetValue(i).GetString(), expected_data[i]);
  }
}

TEST_F(VectorOperationsCastTest, TryCastDecimalIntoDecimal1) {
  std::vector<int32_t> data = {2, 11, 111, 100000};
  std::vector<std::string> expected_data = {"0.0200", "0.1100", "1.1100", "1000.0000"};

  auto scale = 2;
  auto result_scale = 4;
  LogicalType source_type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, scale);
  LogicalType result_type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, result_scale);
  ASSERT_EQ(result_type.GetPhysicalType(), PhysicalType::BIGINT);
  Vector input1 = GenerateRawVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type.GetPhysicalType());

  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  Vector result2(LogicalTypeId::STRING, data.size());
  VectorOperations::TryCast(result1, result2, data.size(), error.get());

  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result2.GetValue(i).GetString(), expected_data[i]);
  }
}

TEST_F(VectorOperationsCastTest, TryCastDecimalIntoDecimal2) {
  std::vector<int32_t> data = {2, 11, 111, 100000};
  std::vector<std::string> expected_data = {"0.00", "0.00", "0.01", "10.00"};

  auto scale = 4;
  auto result_scale = 2;
  LogicalType source_type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, scale);
  LogicalType result_type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, result_scale);
  ASSERT_EQ(result_type.GetPhysicalType(), PhysicalType::BIGINT);
  Vector input1 = GenerateRawVector(source_type, data);
  EXPECT_EQ(input1.GetType(), source_type.GetPhysicalType());

  Vector result1(result_type, data.size());
  auto error = std::make_unique<std::string>();
  bool r = VectorOperations::TryCast(input1, result1, data.size(), error.get());
  EXPECT_EQ(r, true);
  EXPECT_EQ(error->size(), 0);
  Vector result2(LogicalTypeId::STRING, data.size());
  VectorOperations::TryCast(result1, result2, data.size(), error.get());

  for (idx_t i = 0; i < data.size(); i++) {
    EXPECT_EQ(result2.GetValue(i).GetString(), expected_data[i]);
  }
}

// A failed conversion under Cast() (the non-Try form) must turn the row into a genuine NULL —
// not the NullValue<T>() sentinel, which surfaces as a plausible 0 / INT_MIN and corrupts the
// read. The successful rows are unaffected.
TEST_F(VectorOperationsCastTest, CastOverflowRowBecomesNull) {
  std::vector<uint64_t> data = {1, 65536, 7};  // 65536 does not fit a USMALLINT
  Vector input = GenerateVector(PhysicalType::UBIGINT, data);
  Vector result(PhysicalType::USMALLINT, data.size());

  VectorOperations::Cast(input, result, data.size());

  EXPECT_FALSE(result.GetValue(0).IsNull());
  EXPECT_EQ(result.GetValue(0).GetAs<uint16_t>(), 1);
  EXPECT_TRUE(result.GetValue(1).IsNull()) << "the overflowing row must be NULL, not a sentinel value";
  EXPECT_FALSE(result.GetValue(2).IsNull());
  EXPECT_EQ(result.GetValue(2).GetAs<uint16_t>(), 7);
}

// A string that does not parse becomes NULL, and the parseable rows around it survive.
TEST_F(VectorOperationsCastTest, CastUnparseableStringBecomesNull) {
  std::vector<std::string> data = {"42", "12a3", "7"};
  Vector input = GenerateVector(PhysicalType::STRING, data);
  Vector result(PhysicalType::INTEGER, data.size());

  VectorOperations::Cast(input, result, data.size());

  EXPECT_EQ(result.GetValue(0).GetAs<int32_t>(), 42);
  EXPECT_TRUE(result.GetValue(1).IsNull()) << "the unparseable row must be NULL, not INT_MIN";
  EXPECT_EQ(result.GetValue(2).GetAs<int32_t>(), 7);
}

// A CONSTANT input that overflows must collapse to a single NULL, exercising the constant
// path of the failure handling.
TEST_F(VectorOperationsCastTest, CastConstantOverflowBecomesNull) {
  Vector input(Value(static_cast<int32_t>(70000)));  // does not fit a USMALLINT
  Vector result(PhysicalType::USMALLINT, 4);

  VectorOperations::Cast(input, result, 4);
  for (idx_t i = 0; i < 4; i++) {
    EXPECT_TRUE(result.GetValue(i).IsNull()) << "row " << i;
  }
}

// A CONSTANT input that fits casts to a CONSTANT result — the encoding must survive the cast.
TEST_F(VectorOperationsCastTest, CastConstantInput) {
  Vector input(Value(static_cast<int32_t>(300)));
  Vector result(PhysicalType::BIGINT, 4);

  VectorOperations::Cast(input, result, 4);
  EXPECT_EQ(result.GetVectorType(), VectorType::CONSTANT_VECTOR);
  EXPECT_EQ(result.GetValue(0).GetAs<int64_t>(), 300);
}

// A DICTIONARY input is orrified inside the cast; the values must come out in selection order.
TEST_F(VectorOperationsCastTest, CastDictionaryInput) {
  Vector base(PhysicalType::INTEGER, 6);
  for (idx_t i = 0; i < 6; i++) {
    base.SetValue(i, Value(static_cast<int32_t>((i + 1) * 10)));
  }
  SelectionVector sel(3);
  sel.SetIndex(0, 5);  // 60
  sel.SetIndex(1, 1);  // 20
  sel.SetIndex(2, 3);  // 40
  Vector dict(base, sel, 3);

  Vector result(PhysicalType::BIGINT, 3);
  VectorOperations::Cast(dict, result, 3);
  EXPECT_EQ(result.GetValue(0).GetAs<int64_t>(), 60);
  EXPECT_EQ(result.GetValue(1).GetAs<int64_t>(), 20);
  EXPECT_EQ(result.GetValue(2).GetAs<int64_t>(), 40);
}

// A NULL source row stays NULL through a cast (the source validity is propagated), and a
// failing cast does not report a spurious error for the skipped NULL row.
TEST_F(VectorOperationsCastTest, CastPreservesSourceNull) {
  Vector input(PhysicalType::INTEGER, 3);
  input.SetValue(0, Value(5));
  input.SetValue(1, Value::Null(PhysicalType::INTEGER));
  input.SetValue(2, Value(9));
  Vector result(PhysicalType::BIGINT, 3);

  VectorOperations::Cast(input, result, 3);
  EXPECT_EQ(result.GetValue(0).GetAs<int64_t>(), 5);
  EXPECT_TRUE(result.GetValue(1).IsNull());
  EXPECT_EQ(result.GetValue(2).GetAs<int64_t>(), 9);
}

// TryCast reports failure but still converts the rows that do fit, NULLing only the bad one.
TEST_F(VectorOperationsCastTest, TryCastReportsFailureButConvertsGoodRows) {
  std::vector<uint64_t> data = {1, 2, 70000, 3};
  Vector input = GenerateVector(PhysicalType::UBIGINT, data);
  Vector result(PhysicalType::USMALLINT, data.size());
  auto error = std::make_unique<std::string>();

  bool ok = VectorOperations::TryCast(input, result, data.size(), error.get());
  EXPECT_FALSE(ok);
  EXPECT_FALSE(error->empty());
  EXPECT_EQ(result.GetValue(0).GetAs<uint16_t>(), 1);
  EXPECT_EQ(result.GetValue(1).GetAs<uint16_t>(), 2);
  EXPECT_TRUE(result.GetValue(2).IsNull());
  EXPECT_EQ(result.GetValue(3).GetAs<uint16_t>(), 3);
}

}  // namespace bumblebee
