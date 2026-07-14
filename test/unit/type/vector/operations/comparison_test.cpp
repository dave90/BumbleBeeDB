//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// comparison_test.cpp
//
// Identification: test/unit/type/vector/operations/comparison_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <vector>

#include "common/config.h"
#include "gtest/gtest.h"
#include "null_test_base.h"
#include "type/decimal.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

class VectorOperationsComparisonTest : public BumbleBaseTest {};

TEST_F(VectorOperationsComparisonTest, CompareEqualFlatVectors) {
  std::vector<int32_t> values = {1, 2, 3, 4};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, values);
  Vector v2 = GenerateVector(PhysicalType::INTEGER, values);

  auto match = VectorOperations::Equals(v1, v2, nullptr, values.size(), nullptr);
  // Every row is expected to match.
  EXPECT_EQ(match, values.size());

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  match = VectorOperations::Equals(v1, v2, nullptr, values.size(), &sel);
  EXPECT_EQ(match, values.size());
  // Every index is expected to be in the selection.
  for (idx_t i = 0; i < match; i++) {
    EXPECT_EQ(sel[i], i);
  }
}

TEST_F(VectorOperationsComparisonTest, CompareEqualFlatVectorsWithSel) {
  std::vector<int32_t> values = {1, 2, 3, 4};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, values);
  Vector v2 = GenerateVector(PhysicalType::INTEGER, values);

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  SelectionVector sel_result(STANDARD_VECTOR_SIZE);
  sel_result.SetIndex(0, 3);
  sel_result.SetIndex(1, 2);
  sel_result.SetIndex(2, 1);
  sel_result.SetIndex(3, 0);

  auto match = VectorOperations::Equals(v1, v2, &sel_result, values.size(), &sel);
  EXPECT_EQ(match, values.size());
}

TEST_F(VectorOperationsComparisonTest, CompareNotEqualFlatVectors) {
  std::vector<int32_t> values = {1, 2, 3, 4};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, values);
  Vector v2 = GenerateVector(PhysicalType::INTEGER, values);

  auto match = VectorOperations::NotEquals(v1, v2, nullptr, values.size(), nullptr);
  EXPECT_EQ(match, 0);
}

TEST_F(VectorOperationsComparisonTest, CompareEqualFlatVectorsDifferentTypes) {
  std::vector<int32_t> values1 = {1, 2, 3, 4};
  std::vector<uint16_t> values2 = {1, 2, 3, 4};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, values1);
  Vector v2 = GenerateVector(PhysicalType::USMALLINT, values2);

  auto match = VectorOperations::Equals(v1, v2, nullptr, values1.size(), nullptr);
  EXPECT_EQ(match, values1.size());
}

TEST_F(VectorOperationsComparisonTest, SignedVsUnsignedComparison) {
  std::vector<int32_t> signed_values = {-1, 0, 1, 4};
  std::vector<uint32_t> unsigned_values = {0, 0, 1, 3};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, signed_values);
  Vector v2 = GenerateVector(PhysicalType::UINTEGER, unsigned_values);

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  auto match = VectorOperations::GreaterThan(v1, v2, nullptr, signed_values.size(), &sel);
  // Only 4 > 3. Crucially -1 > 0 must be FALSE: promoting both to int64 is what keeps the
  // C usual-arithmetic-conversion answer (which would be TRUE) out of the engine.
  EXPECT_EQ(match, 1);
}

TEST_F(VectorOperationsComparisonTest, FloatVsIntegerComparison) {
  std::vector<float> float_values = {1.0F, 2.5F, 3.0F, 4.1F};
  std::vector<int32_t> int_values = {1, 3, 2, 4};
  Vector v1 = GenerateVector(PhysicalType::FLOAT, float_values);
  Vector v2 = GenerateVector(PhysicalType::INTEGER, int_values);

  auto match = VectorOperations::LessThanEquals(v1, v2, nullptr, float_values.size(), nullptr);
  EXPECT_EQ(match, 2);  // 1.0 <= 1 and 2.5 <= 3; 3.0 > 2 and 4.1 > 4
}

TEST_F(VectorOperationsComparisonTest, MixedTypesEdgeValues) {
  std::vector<int8_t> values1 = {0, INT8_MAX, 100, INT8_MAX};
  std::vector<uint16_t> values2 = {0, 0, 100, 200};
  Vector v1 = GenerateVector(PhysicalType::TINYINT, values1);
  Vector v2 = GenerateVector(PhysicalType::USMALLINT, values2);

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  auto match = VectorOperations::GreaterThan(v1, v2, nullptr, values1.size(), &sel);
  EXPECT_EQ(match, 1);  // INT8_MAX > 0
  EXPECT_EQ(sel.GetIndex(0), 1);
}

TEST_F(VectorOperationsComparisonTest, CompareEqualStringVectors) {
  std::vector<const char *> values = {"apple", "banana", "miao", "meowmeowmeowmeowmeowmeow"};
  Vector v1 = GenerateVector(PhysicalType::STRING, values);
  Vector v2 = GenerateVector(PhysicalType::STRING, values);

  auto match = VectorOperations::Equals(v1, v2, nullptr, values.size(), nullptr);
  EXPECT_EQ(match, values.size());

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  match = VectorOperations::Equals(v1, v2, nullptr, values.size(), &sel);
  EXPECT_EQ(match, values.size());
  for (idx_t i = 0; i < match; i++) {
    EXPECT_EQ(sel[i], i);
  }
}

TEST_F(VectorOperationsComparisonTest, CompareLessThanStringVectors) {
  std::vector<const char *> v1_values = {"ant", "bat", "miao", "dog"};
  std::vector<const char *> v2_values = {"apple", "banana", "meow", "donkey"};

  Vector v1 = GenerateVector(PhysicalType::STRING, v1_values);
  Vector v2 = GenerateVector(PhysicalType::STRING, v2_values);

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  auto match = VectorOperations::LessThan(v1, v2, nullptr, v1_values.size(), &sel);
  // "ant" < "apple" and "dog" < "donkey"; "bat" > "banana" and "miao" > "meow".
  EXPECT_EQ(match, 2);
}

TEST_F(VectorOperationsComparisonTest, SignedVsDecimalComparison) {
  std::vector<int32_t> signed_values = {-1, 0, 1, 4};
  std::vector<int64_t> decimal_values = {0, 100, 10, 1002};
  Vector v1 = GenerateVector(PhysicalType::INTEGER, signed_values);
  Vector v2 = GenerateRawVector(LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, 2), decimal_values);

  SelectionVector sel(STANDARD_VECTOR_SIZE);
  auto match1 = VectorOperations::GreaterThan(v1, v2, nullptr, signed_values.size(), &sel);
  auto match2 = VectorOperations::LessThan(v2, v1, nullptr, signed_values.size(), &sel);
  // `a > b` and `b < a` have to agree, whichever side the DECIMAL is on.
  EXPECT_EQ(match1, match2);
}

// ---------------------------------------------------------------------------
// NULL semantics.
// ---------------------------------------------------------------------------

class ComparisonNullTest : public NullTestBase {};

/** @return The indices in `sel`, sorted, so that they can be compared as a set. */
static auto ToVec(const SelectionVector &sel, idx_t n) -> std::vector<idx_t> {
  std::vector<idx_t> out;
  out.reserve(n);
  for (idx_t i = 0; i < n; i++) {
    out.push_back(sel.GetIndex(i));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Equals(left, right, sel, count, true_sel): a NULL row must never reach true_sel.
TEST_F(ComparisonNullTest, EqualsExcludesNullFromTrueSel) {
  // left:  [10, 20, 30, NULL, 50]
  // right: [10, 25, 30, 40,   NULL]
  // Expected matches: row 0 and row 2. Rows 3 and 4 are NULL on at least one side.
  Vector left = CreateVectorWithNulls(PhysicalType::INTEGER, 5, {3});
  Vector right(PhysicalType::INTEGER, 5);
  right.SetValue(0, Value(static_cast<int32_t>(10)));
  right.SetValue(1, Value(static_cast<int32_t>(25)));
  right.SetValue(2, Value(static_cast<int32_t>(30)));
  right.SetValue(3, Value(static_cast<int32_t>(40)));
  right.SetValue(4, Value::Null(PhysicalType::INTEGER));
  // Overwrite the left rows 0 and 2 so that they match.
  left.SetValue(0, Value(static_cast<int32_t>(10)));
  left.SetValue(2, Value(static_cast<int32_t>(30)));

  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  auto true_count = VectorOperations::Equals(left, right, nullptr, 5, &true_sel);
  EXPECT_EQ(true_count, 2U);
  auto idxs = ToVec(true_sel, true_count);
  EXPECT_EQ(idxs, (std::vector<idx_t>{0, 2}));
}

// The 7-argument overload returns both selections. An UNKNOWN row goes to false_sel, so
// that an OR-eval can hand it to the next branch.
TEST_F(ComparisonNullTest, EqualsUnknownGoesToFalseSelNotTrueSel) {
  // a = [1, 2, NULL, 4]; b = [1, 5, 3, NULL]
  Vector a(PhysicalType::INTEGER, 4);
  a.SetValue(0, Value(1));
  a.SetValue(1, Value(2));
  a.SetValue(2, Value::Null(PhysicalType::INTEGER));
  a.SetValue(3, Value(4));
  Vector b(PhysicalType::INTEGER, 4);
  b.SetValue(0, Value(1));
  b.SetValue(1, Value(5));
  b.SetValue(2, Value(3));
  b.SetValue(3, Value::Null(PhysicalType::INTEGER));

  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  SelectionVector false_sel(STANDARD_VECTOR_SIZE);
  idx_t false_count = 0;
  auto true_count = VectorOperations::Equals(a, b, nullptr, 4, &true_sel, &false_sel, false_count);
  // Only row 0 matches; the rest — including the two NULL rows — belong to false_sel, so
  // that the OR-eval invariant `true_count + false_count == count` holds.
  EXPECT_EQ(true_count, 1U);
  EXPECT_EQ(false_count, 3U);
  EXPECT_EQ(true_sel.GetIndex(0), 0U);
  auto false_idxs = ToVec(false_sel, false_count);
  EXPECT_EQ(false_idxs, (std::vector<idx_t>{1, 2, 3}));
}

// `a = 5 OR b = 10` where A is NULL on a row that B matches: the OR-eval pattern. Iteration
// 1 leaves the NULL row in false_sel; iteration 2 evaluates B on it and matches.
TEST_F(ComparisonNullTest, OrEvalKeepsNullRowAliveForNextBranch) {
  // a: [NULL, 5,  1]
  // b: [10,   20, 10]
  Vector a(PhysicalType::INTEGER, 3);
  a.SetValue(0, Value::Null(PhysicalType::INTEGER));
  a.SetValue(1, Value(5));
  a.SetValue(2, Value(1));
  Vector b(PhysicalType::INTEGER, 3);
  b.SetValue(0, Value(10));
  b.SetValue(1, Value(20));
  b.SetValue(2, Value(10));
  Vector five(Value(5));
  Vector ten(Value(10));

  // Iteration 1: a == 5.
  SelectionVector sel = FlatVector::INCREMENTAL_SELECTION_VECTOR;
  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  SelectionVector false_sel(STANDARD_VECTOR_SIZE);
  idx_t false_count = 0;
  idx_t count = 3;
  auto true_count = VectorOperations::Equals(a, five, &sel, count, &true_sel, &false_sel, false_count);
  // Row 1 matches; rows 0 (NULL) and 2 do not, so both land in false_sel.
  EXPECT_EQ(true_count, 1U);
  EXPECT_EQ(false_count, 2U);

  // Iteration 2: b == 10, over the false_sel rows.
  SelectionVector true_sel2(STANDARD_VECTOR_SIZE);
  SelectionVector false_sel2(STANDARD_VECTOR_SIZE);
  idx_t false_count2 = 0;
  auto true_count2 = VectorOperations::Equals(b, ten, &false_sel, false_count, &true_sel2, &false_sel2, false_count2);
  // Rows 0 and 2 both match: the NULL on `a` did not eliminate row 0 from the disjunction.
  EXPECT_EQ(true_count2, 2U);
  auto matched = ToVec(true_sel2, true_count2);
  EXPECT_EQ(matched, (std::vector<idx_t>{0, 2}));
}

// NULL > x is UNKNOWN.
TEST_F(ComparisonNullTest, GreaterThanExcludesNull) {
  // CreateVectorWithNulls fills v[i] = i + 1, then nulls the listed rows: [1, NULL, 3, NULL, 5].
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 5, {1, 3});
  Vector two(Value(static_cast<int32_t>(2)));
  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  auto true_count = VectorOperations::GreaterThan(v, two, nullptr, 5, &true_sel);
  // > 2 yields false, UNKNOWN, true, UNKNOWN, true.
  EXPECT_EQ(true_count, 2U);
  auto idxs = ToVec(true_sel, true_count);
  EXPECT_EQ(idxs, (std::vector<idx_t>{2, 4}));
}

// A NULL constant on either side makes every row UNKNOWN.
TEST_F(ComparisonNullTest, NullConstantOperandFiltersEverything) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 4, {});
  Vector null_const(Value::Null(PhysicalType::INTEGER));

  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  SelectionVector false_sel(STANDARD_VECTOR_SIZE);
  idx_t false_count = 0;
  auto true_count = VectorOperations::Equals(v, null_const, nullptr, 4, &true_sel, &false_sel, false_count);
  EXPECT_EQ(true_count, 0U);
  EXPECT_EQ(false_count, 4U);  // every row survives into false_sel as UNKNOWN
}

// More than one vector's worth of rows, with sparse nulls.
TEST_F(ComparisonNullTest, MultiBatchSparseNulls) {
  const idx_t count = 6000;
  auto positions = RandomNullPlacement(count, 0.05, 7);
  Vector left(PhysicalType::BIGINT, count);
  Vector right(PhysicalType::BIGINT, count);
  for (idx_t i = 0; i < count; i++) {
    left.SetValue(i, Value(static_cast<int64_t>(i)));
    right.SetValue(i, Value(static_cast<int64_t>(i)));  // equal everywhere
  }
  for (auto p : positions) {
    // Null on the left only: these rows would otherwise have compared equal.
    left.SetValue(p, Value::Null(PhysicalType::BIGINT));
  }

  SelectionVector true_sel(count + 16);
  auto true_count = VectorOperations::Equals(left, right, nullptr, count, &true_sel);
  EXPECT_EQ(true_count, count - positions.size());
}

// IS NOT DISTINCT FROM: two NULLs match; a NULL and a non-NULL do not.
TEST_F(ComparisonNullTest, NotDistinctFromTreatsBothNullAsEqual) {
  Vector a(PhysicalType::INTEGER, 4);
  a.SetValue(0, Value(1));
  a.SetValue(1, Value::Null(PhysicalType::INTEGER));
  a.SetValue(2, Value::Null(PhysicalType::INTEGER));
  a.SetValue(3, Value(7));
  Vector b(PhysicalType::INTEGER, 4);
  b.SetValue(0, Value(1));                             // equal
  b.SetValue(1, Value::Null(PhysicalType::INTEGER));   // both NULL -> NOT DISTINCT
  b.SetValue(2, Value(2));                             // NULL vs 2 -> distinct
  b.SetValue(3, Value(8));                             // 7 vs 8 -> distinct

  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  auto matched = VectorOperations::NotDistinctFrom(a, b, nullptr, 4, &true_sel);
  auto idxs = ToVec(true_sel, matched);
  EXPECT_EQ(idxs, (std::vector<idx_t>{0, 1}));

  // DistinctFrom is the exact inverse over the same input.
  SelectionVector distinct_sel(STANDARD_VECTOR_SIZE);
  auto distinct_count = VectorOperations::DistinctFrom(a, b, nullptr, 4, &distinct_sel);
  auto distinct_idxs = ToVec(distinct_sel, distinct_count);
  EXPECT_EQ(distinct_idxs, (std::vector<idx_t>{2, 3}));
}

// IS NULL / IS NOT NULL: pure mask reads, independent of the values.
TEST_F(ComparisonNullTest, IsNullAndIsNotNull) {
  Vector v = CreateVectorWithNulls(PhysicalType::BIGINT, 6, {1, 4});

  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  auto null_count = VectorOperations::IsNull(v, nullptr, 6, &true_sel);
  auto null_idxs = ToVec(true_sel, null_count);
  EXPECT_EQ(null_idxs, (std::vector<idx_t>{1, 4}));

  SelectionVector true_sel2(STANDARD_VECTOR_SIZE);
  auto not_null_count = VectorOperations::IsNotNull(v, nullptr, 6, &true_sel2);
  auto not_null_idxs = ToVec(true_sel2, not_null_count);
  EXPECT_EQ(not_null_idxs, (std::vector<idx_t>{0, 2, 3, 5}));
}

// A dictionary input goes through the generic select path, which reads validity through the
// selection. A null behind the selection must still be excluded.
TEST_F(ComparisonNullTest, EqualsOverDictionaryInput) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {2});
  v.SetValue(0, Value(10));
  v.SetValue(1, Value(20));
  // v[2] is NULL.
  v.SetValue(3, Value(30));
  v.SetValue(4, Value(40));
  v.SetValue(5, Value(50));
  SelectionVector sel(4);
  sel.SetIndex(0, 0);  // 10
  sel.SetIndex(1, 2);  // NULL
  sel.SetIndex(2, 3);  // 30
  sel.SetIndex(3, 4);  // 40
  Vector dict(v, sel, 4);
  ASSERT_EQ(dict.GetVectorType(), VectorType::DICTIONARY_VECTOR);

  Vector ten(Value(10));
  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  auto matched = VectorOperations::Equals(dict, ten, nullptr, 4, &true_sel);
  EXPECT_EQ(matched, 1U);
  EXPECT_EQ(true_sel.GetIndex(0), 0U);
}

// The DECIMAL path casts one side before comparing. A NULL must survive that cast.
TEST_F(ComparisonNullTest, DecimalComparisonHonorsNull) {
  LogicalType dec_type = LogicalType::Decimal(10, 2);
  Vector dec(dec_type, 4);
  // SetValue writes the value straight into the DECIMAL's backing integer.
  dec.SetValue(0, Value(static_cast<int64_t>(100)).CastAs(dec_type.GetPhysicalType()));  // 1.00
  dec.SetValue(1, Value(static_cast<int64_t>(200)).CastAs(dec_type.GetPhysicalType()));  // 2.00
  dec.SetValue(2, Value::Null(dec_type));
  dec.SetValue(3, Value(static_cast<int64_t>(300)).CastAs(dec_type.GetPhysicalType()));  // 3.00

  Vector two(Value(static_cast<int32_t>(2)));
  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  auto true_count = VectorOperations::GreaterThan(dec, two, nullptr, 4, &true_sel);
  // > 2.00 matches row 3 only. Row 2 is NULL and must be excluded.
  EXPECT_EQ(true_count, 1U);
  EXPECT_EQ(true_sel.GetIndex(0), 3U);
}

// Two DECIMALs of DIFFERENT scales share a physical type (both BIGINT) but their raw
// integers are not comparable: 1.00 is stored as 100 and 1.0 as 10. The kernel must bring
// them onto a common scale first.
TEST_F(ComparisonNullTest, DecimalVsDecimalDifferentScales) {
  LogicalType t2 = LogicalType::Decimal(18, 2);
  LogicalType t1 = LogicalType::Decimal(18, 1);
  Vector a(t2, 3);
  Vector b(t1, 3);
  // a = [1.00, 2.00, 3.00] -> raw 100, 200, 300
  a.SetValue(0, Value(static_cast<int64_t>(100)).CastAs(t2.GetPhysicalType()));
  a.SetValue(1, Value(static_cast<int64_t>(200)).CastAs(t2.GetPhysicalType()));
  a.SetValue(2, Value(static_cast<int64_t>(300)).CastAs(t2.GetPhysicalType()));
  // b = [1.0, 5.0, 1.0] -> raw 10, 50, 10
  b.SetValue(0, Value(static_cast<int64_t>(10)).CastAs(t1.GetPhysicalType()));
  b.SetValue(1, Value(static_cast<int64_t>(50)).CastAs(t1.GetPhysicalType()));
  b.SetValue(2, Value(static_cast<int64_t>(10)).CastAs(t1.GetPhysicalType()));

  SelectionVector true_sel(STANDARD_VECTOR_SIZE);
  // 1.00 == 1.0 (row 0) and 3.00 != 1.0; only row 0 is equal.
  auto eq = VectorOperations::Equals(a, b, nullptr, 3, &true_sel);
  EXPECT_EQ(eq, 1U);
  EXPECT_EQ(true_sel.GetIndex(0), 0U);

  // 3.00 > 1.0 -> row 2 only (1.00 > 1.0 is false, 2.00 > 5.0 is false).
  SelectionVector gt_sel(STANDARD_VECTOR_SIZE);
  auto gt = VectorOperations::GreaterThan(a, b, nullptr, 3, &gt_sel);
  EXPECT_EQ(gt, 1U);
  EXPECT_EQ(gt_sel.GetIndex(0), 2U);
}

}  // namespace bumblebee
