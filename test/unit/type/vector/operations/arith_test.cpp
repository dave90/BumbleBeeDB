//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// arith_test.cpp
//
// Identification: test/unit/type/vector/operations/arith_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <vector>

#include "common/config.h"
#include "common/numeric_utils.h"
#include "gtest/gtest.h"
#include "null_test_base.h"
#include "type/decimal.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

class VectorOperationsArithTest : public BumbleBaseTest {};

TEST_F(VectorOperationsArithTest, SumFlatVectors) {
  std::vector<int32_t> values = {1, 2, 3, 4};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, values);
  Vector v2 = GenerateVector(PhysicalType::INTEGER, values);
  Vector v3(PhysicalType::INTEGER);

  VectorOperations::Sum(v1, v2, v3, values.size());
  for (idx_t i = 0; i < values.size(); i++) {
    EXPECT_EQ(values[i] + values[i], v3.GetValue(i).GetAs<int32_t>());
  }
}

TEST_F(VectorOperationsArithTest, DifferenceFlatVectors) {
  std::vector<int16_t> values1 = {10, 20, -30, 40};
  std::vector<int16_t> values2 = {1, 5, -10, 50};
  Vector v1 = GenerateVector(PhysicalType::SMALLINT, values1);
  Vector v2 = GenerateVector(PhysicalType::SMALLINT, values2);
  Vector v3(PhysicalType::SMALLINT);

  VectorOperations::Difference(v1, v2, v3, values1.size());
  for (idx_t i = 0; i < values1.size(); i++) {
    EXPECT_EQ(values1[i] - values2[i], v3.GetValue(i).GetAs<int16_t>());
  }
}

TEST_F(VectorOperationsArithTest, DotCrossTypeVectors) {
  std::vector<uint32_t> values1 = {10, 20, 30, 40};
  std::vector<float> values2 = {1.5F, -2.5F, 3.0F, 0.5F};
  Vector v1 = GenerateVector(PhysicalType::UINTEGER, values1);
  Vector v2 = GenerateVector(PhysicalType::FLOAT, values2);
  Vector v3(PhysicalType::DOUBLE);  // the result type has to be wide enough

  VectorOperations::Dot(v1, v2, v3, values1.size());

  for (idx_t i = 0; i < values1.size(); i++) {
    EXPECT_EQ(static_cast<double>(values1[i]) * static_cast<double>(values2[i]), v3.GetValue(i).GetAs<double>());
  }
}

// The result type is the LEFT vector's, since the dispatch is on the left.
TEST_F(VectorOperationsArithTest, AndCrossTypeVectors) {
  std::vector<uint32_t> left_vals = {
      0xFFFFFFFFU,  // all 32 bits set
      0xABCDEF01U,  // a mixed pattern, high bits set
      0x0000FFFFU,  // the lower 16 bits set, the upper 16 clear
      0x13579BDFU   // a checkerboard-ish pattern
  };
  // Values that fit in an int16_t: 0x00FF -> 255, 0x0F0F -> 3855, 0xF0F0 -> -3856, and 1.
  std::vector<int16_t> right_vals = {255, 3855, -3856, 1};

  Vector v_left = GenerateVector(PhysicalType::UINTEGER, left_vals);
  Vector v_right = GenerateVector(PhysicalType::SMALLINT, right_vals);
  Vector v_out(PhysicalType::UINTEGER);  // wide enough, and aligned with the LEFT type

  VectorOperations::LAnd(v_left, v_right, v_out, left_vals.size());

  for (idx_t i = 0; i < left_vals.size(); i++) {
    // Cast the RIGHT operand into the LEFT's representation before the &, mirroring what
    // the left-dispatched implementation effectively does.
    auto rhs_as_left = static_cast<uint32_t>(static_cast<uint16_t>(right_vals[i]));
    uint32_t expected = left_vals[i] & rhs_as_left;
    EXPECT_EQ(expected, v_out.GetValue(i).GetAs<uint32_t>()) << "mismatch at index " << i;
  }
}

TEST_F(VectorOperationsArithTest, AndCrossTypeConstantVectors) {
  std::vector<uint32_t> left_vals = {0xFFFFFFFFU, 0xABCDEF01U, 0x0000FFFFU, 0x13579BDFU};
  int16_t right_val = 255;

  Vector v_left = GenerateVector(PhysicalType::UINTEGER, left_vals);
  Value right_value(right_val);
  Vector v_right(right_value);
  EXPECT_EQ(v_right.GetVectorType(), VectorType::CONSTANT_VECTOR);

  Vector v_out(PhysicalType::UINTEGER);

  VectorOperations::LAnd(v_left, v_right, v_out, left_vals.size());

  for (idx_t i = 0; i < left_vals.size(); i++) {
    uint32_t expected = left_vals[i] & static_cast<uint32_t>(static_cast<uint16_t>(right_val));
    EXPECT_EQ(expected, v_out.GetValue(i).GetAs<uint32_t>()) << "mismatch at index " << i;
  }
}

TEST_F(VectorOperationsArithTest, NegateIntVectors) {
  std::vector<int16_t> values1 = {10, 20, -30, 40};
  Vector v1 = GenerateVector(PhysicalType::SMALLINT, values1);
  Vector v3(PhysicalType::SMALLINT);

  VectorOperations::Negate(v1, v3, values1.size());
  for (idx_t i = 0; i < values1.size(); i++) {
    EXPECT_EQ(values1[i] * -1, v3.GetValue(i).GetAs<int16_t>());
  }
}

TEST_F(VectorOperationsArithTest, NegateDecimalVectors) {
  std::vector<int16_t> values1 = {10, 20, -30, 40};
  int scale = 2;
  auto type = LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT16, scale);
  Vector v1 = GenerateRawVector(type, values1);
  Vector v3(type);

  VectorOperations::Negate(v1, v3, values1.size());
  Vector v4(LogicalTypeId::STRING);
  VectorOperations::TryCast(v3, v4, values1.size(), nullptr);
  for (idx_t i = 0; i < values1.size(); i++) {
    EXPECT_EQ(Decimal::ToString(static_cast<int16_t>(values1[i] * -1), scale), v4.GetValue(i).GetString());
  }
}

TEST_F(VectorOperationsArithTest, DecimalMultiplyVectors) {
  std::vector<int16_t> values1 = {1, 10, 100, 1000};
  std::vector<int16_t> values2 = {10, 100, 1000, 10000};
  int scale1 = 2;
  int scale2 = 3;
  int result_scale = 5;
  Vector v1 = GenerateRawVector(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, scale1), values1);
  Vector v2 = GenerateRawVector(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, scale2), values2);
  Vector result_vec(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, result_scale));

  VectorOperations::Dot(v1, v2, result_vec, values1.size());
  Vector result_string_vec(LogicalTypeId::STRING);
  VectorOperations::TryCast(result_vec, result_string_vec, values1.size(), nullptr);
  for (idx_t i = 0; i < values1.size(); i++) {
    // The raw product already carries scale1 + scale2 == result_scale, so no rescaling.
    EXPECT_EQ(Decimal::ToString(static_cast<int64_t>(values1[i]) * values2[i], result_scale),
              result_string_vec.GetValue(i).GetString());
  }
}

TEST_F(VectorOperationsArithTest, DecimalDivisionDifferentScales) {
  // Values chosen to divide cleanly, so the truncation policy is not in question.
  // left:  12.34 (scale 2, raw 1234); right: 2.0 (scale 1, raw 20); result scale 2.
  std::vector<int32_t> left_vals = {1234, -1234, 500, -500};  // scale 2
  std::vector<int32_t> right_vals = {20, 20, 10, -10};        // scale 1

  int left_scale = 2;
  int right_scale = 1;
  int result_scale = 2;

  Vector v_left = GenerateRawVector(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, left_scale), left_vals);
  Vector v_right =
      GenerateRawVector(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, right_scale), right_vals);
  Vector v_out(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, result_scale));

  VectorOperations::Division(v_left, v_right, v_out, left_vals.size());

  Vector out_str(LogicalTypeId::STRING);
  VectorOperations::TryCast(v_out, out_str, left_vals.size(), nullptr);

  for (idx_t i = 0; i < left_vals.size(); i++) {
    // expected_raw = (L * 10^(result_scale + right_scale - left_scale)) / R
    int exp = result_scale + right_scale - left_scale;

    __int128 num = static_cast<__int128>(left_vals[i]);
    __int128 den = static_cast<__int128>(right_vals[i]);
    if (exp >= 0) {
      num *= NumericHelper::POWERS_OF_TEN[exp];
    } else {
      num /= NumericHelper::POWERS_OF_TEN[-exp];
    }
    auto expected_raw = static_cast<int64_t>(num / den);
    EXPECT_EQ(Decimal::ToString(expected_raw, result_scale), out_str.GetValue(i).GetString())
        << "mismatch at index " << i;
  }
}

// ---------------------------------------------------------------------------
// NULL semantics.
// ---------------------------------------------------------------------------

class ArithNullTest : public NullTestBase {};

// a + b: a NULL on either side yields a NULL result.
TEST_F(ArithNullTest, AdditionPropagatesNull) {
  Vector a(PhysicalType::INTEGER, 4);
  a.SetValue(0, Value(1));
  a.SetValue(1, Value::Null(PhysicalType::INTEGER));
  a.SetValue(2, Value(3));
  a.SetValue(3, Value(4));
  Vector b(PhysicalType::INTEGER, 4);
  b.SetValue(0, Value(10));
  b.SetValue(1, Value(20));
  b.SetValue(2, Value::Null(PhysicalType::INTEGER));
  b.SetValue(3, Value(40));

  Vector r(PhysicalType::INTEGER, 4);
  VectorOperations::Sum(a, b, r, 4);

  EXPECT_FALSE(IsNull(r, 0));
  EXPECT_EQ(r.GetValue(0), Value(11));
  EXPECT_TRUE(IsNull(r, 1));  // NULL + 20
  EXPECT_TRUE(IsNull(r, 2));  // 3 + NULL
  EXPECT_FALSE(IsNull(r, 3));
  EXPECT_EQ(r.GetValue(3), Value(44));
}

// Subtraction and multiplication follow the same rule.
TEST_F(ArithNullTest, AllArithOpsPropagateNull) {
  Vector a(PhysicalType::INTEGER, 3);
  a.SetValue(0, Value(10));
  a.SetValue(1, Value::Null(PhysicalType::INTEGER));
  a.SetValue(2, Value(8));
  Vector b(PhysicalType::INTEGER, 3);
  b.SetValue(0, Value(2));
  b.SetValue(1, Value(5));
  b.SetValue(2, Value::Null(PhysicalType::INTEGER));

  Vector r(PhysicalType::INTEGER, 3);
  VectorOperations::Difference(a, b, r, 3);
  EXPECT_FALSE(IsNull(r, 0));
  EXPECT_TRUE(IsNull(r, 1));
  EXPECT_TRUE(IsNull(r, 2));

  Vector r2(PhysicalType::INTEGER, 3);
  VectorOperations::Dot(a, b, r2, 3);
  EXPECT_FALSE(IsNull(r2, 0));
  EXPECT_TRUE(IsNull(r2, 1));
  EXPECT_TRUE(IsNull(r2, 2));
}

// A non-null constant on one side: the flat side's per-row nulls still propagate.
TEST_F(ArithNullTest, FlatPlusNonNullConstant) {
  Vector a(PhysicalType::INTEGER, 4);
  a.SetValue(0, Value(1));
  a.SetValue(1, Value::Null(PhysicalType::INTEGER));
  a.SetValue(2, Value(3));
  a.SetValue(3, Value::Null(PhysicalType::INTEGER));
  Vector five(Value(static_cast<int32_t>(5)));

  Vector r(PhysicalType::INTEGER, 4);
  VectorOperations::Sum(a, five, r, 4);
  EXPECT_FALSE(IsNull(r, 0));
  EXPECT_TRUE(IsNull(r, 1));
  EXPECT_FALSE(IsNull(r, 2));
  EXPECT_TRUE(IsNull(r, 3));
  EXPECT_EQ(r.GetValue(0), Value(6));
  EXPECT_EQ(r.GetValue(2), Value(8));
}

// A NULL constant on either side: every row becomes NULL.
TEST_F(ArithNullTest, NullConstantOnRightMakesAllNull) {
  Vector a(PhysicalType::INTEGER, 3);
  a.SetValue(0, Value(1));
  a.SetValue(1, Value(2));
  a.SetValue(2, Value(3));
  Vector null_const(Value::Null(PhysicalType::INTEGER));

  Vector r(PhysicalType::INTEGER, 3);
  VectorOperations::Sum(a, null_const, r, 3);
  for (idx_t i = 0; i < 3; i++) {
    EXPECT_TRUE(IsNull(r, i));
  }
}

// A dictionary input goes through the generic execute path; the NULLs must still propagate.
TEST_F(ArithNullTest, AdditionOverDictionaryInput) {
  Vector base(PhysicalType::INTEGER, 5);
  base.SetValue(0, Value(10));
  base.SetValue(1, Value(20));
  base.SetValue(2, Value::Null(PhysicalType::INTEGER));
  base.SetValue(3, Value(40));
  base.SetValue(4, Value(50));
  SelectionVector sel(3);
  sel.SetIndex(0, 0);  // 10
  sel.SetIndex(1, 2);  // NULL
  sel.SetIndex(2, 4);  // 50
  Vector dict_a(base, sel, 3);

  Vector b(PhysicalType::INTEGER, 3);
  b.SetValue(0, Value(1));
  b.SetValue(1, Value(2));
  b.SetValue(2, Value(3));

  Vector r(PhysicalType::INTEGER, 3);
  VectorOperations::Sum(dict_a, b, r, 3);
  EXPECT_FALSE(IsNull(r, 0));
  EXPECT_EQ(r.GetValue(0), Value(11));
  EXPECT_TRUE(IsNull(r, 1));
  EXPECT_FALSE(IsNull(r, 2));
  EXPECT_EQ(r.GetValue(2), Value(53));
}

// More than one vector's worth of rows, with sparse nulls.
TEST_F(ArithNullTest, MultiBatchSparseNulls) {
  const idx_t count = 6000;
  auto positions = RandomNullPlacement(count, 0.05, 11);
  Vector a(PhysicalType::BIGINT, count);
  Vector b(PhysicalType::BIGINT, count);
  for (idx_t i = 0; i < count; i++) {
    a.SetValue(i, Value(static_cast<int64_t>(i)));
    b.SetValue(i, Value(static_cast<int64_t>(1)));
  }
  for (auto p : positions) {
    a.SetValue(p, Value::Null(PhysicalType::BIGINT));
  }

  Vector r(PhysicalType::BIGINT, count);
  VectorOperations::Sum(a, b, r, count);

  std::vector<bool> expected_null(count, false);
  for (auto p : positions) {
    expected_null[p] = true;
  }
  for (idx_t i = 0; i < count; i++) {
    EXPECT_EQ(IsNull(r, i), expected_null[i]) << "row " << i;
  }
}

}  // namespace bumblebee
