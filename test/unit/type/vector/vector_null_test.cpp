//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_null_test.cpp
//
// Identification: test/unit/type/vector/vector_null_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "null_test_base.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

class VectorNullTest : public NullTestBase {};

// ---------- FLAT: set / get / ToString ----------

TEST_F(VectorNullTest, FlatSetGetNull) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 8, {2, 5});
  EXPECT_TRUE(IsNull(v, 2));
  EXPECT_TRUE(IsNull(v, 5));
  EXPECT_FALSE(IsNull(v, 0));
  EXPECT_TRUE(v.GetValue(2).IsNull());
  EXPECT_FALSE(v.GetValue(0).IsNull());
  EXPECT_EQ(v.GetValue(2).ToString(), "NULL");
}

TEST_F(VectorNullTest, SetNullThenOverwriteNonNull) {
  Vector v(PhysicalType::INTEGER, 4);
  v.SetValue(1, Value::Null());
  EXPECT_TRUE(IsNull(v, 1));
  v.SetValue(1, Value(99));
  EXPECT_FALSE(IsNull(v, 1));
  EXPECT_EQ(v.GetValue(1), Value(99));
}

TEST_F(VectorNullTest, ToStringShowsNull) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 3, {1});
  auto s = v.ToString(3);
  EXPECT_NE(s.find("NULL"), std::string::npos);
}

// ---------- Reference ----------

TEST_F(VectorNullTest, ReferenceSharesNull) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {3});
  Vector ref(v);  // reference ctor
  EXPECT_TRUE(IsNull(ref, 3));
  EXPECT_FALSE(IsNull(ref, 2));
}

// ---------- Slice(offset) ----------

TEST_F(VectorNullTest, SliceOffsetShiftsNull) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 8, {3});
  Vector sliced(v, 2);  // offset 2: row 3 -> row 1
  EXPECT_TRUE(IsNull(sliced, 1));
  EXPECT_FALSE(IsNull(sliced, 0));
  EXPECT_TRUE(sliced.GetValue(1).IsNull());
  // The original is untouched.
  EXPECT_TRUE(IsNull(v, 3));
}

// ---------- Slice(sel) -> DICTIONARY ----------

TEST_F(VectorNullTest, SliceSelectionReadsNullThroughChild) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {1, 4});
  SelectionVector sel(3);
  sel.SetIndex(0, 1);  // null
  sel.SetIndex(1, 2);
  sel.SetIndex(2, 4);  // null
  Vector dict(v, sel, 3);
  ASSERT_EQ(dict.GetVectorType(), VectorType::DICTIONARY_VECTOR);
  EXPECT_TRUE(IsNull(dict, 0));
  EXPECT_FALSE(IsNull(dict, 1));
  EXPECT_TRUE(IsNull(dict, 2));
  EXPECT_TRUE(dict.GetValue(0).IsNull());
}

// ---------- Normalify ----------

TEST_F(VectorNullTest, NormalifyDictionaryPreservesNull) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {0, 3});
  SelectionVector sel(3);
  sel.SetIndex(0, 3);  // null
  sel.SetIndex(1, 1);
  sel.SetIndex(2, 0);  // null
  Vector dict(v, sel, 3);
  dict.Normalify(3);
  ASSERT_EQ(dict.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_TRUE(IsNull(dict, 0));
  EXPECT_FALSE(IsNull(dict, 1));
  EXPECT_TRUE(IsNull(dict, 2));
}

TEST_F(VectorNullTest, NormalifyNullConstant) {
  // The original's untyped `Value::null()` default-typed to INTEGER; ours defaults to
  // UNKNOWN, so the type is named explicitly. The constant carries one validity bit.
  Vector v(Value::Null(LogicalTypeId::INTEGER));
  v.SetVectorType(VectorType::CONSTANT_VECTOR);
  EXPECT_TRUE(ConstantVector::IsNull(v));
  v.Normalify(5);
  ASSERT_EQ(v.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < 5; i++) {
    EXPECT_TRUE(IsNull(v, i));
  }
}

TEST_F(VectorNullTest, NormalifyNonNullConstantStaysValid) {
  Vector v(Value(7));
  v.Normalify(5);
  for (idx_t i = 0; i < 5; i++) {
    EXPECT_FALSE(IsNull(v, i));
    EXPECT_EQ(v.GetValue(i), Value(7));
  }
}

// ---------- Orrify ----------

TEST_F(VectorNullTest, OrrifyFlatCarriesValidity) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 5, {2});
  VectorData vdata;
  v.Orrify(5, vdata);
  ASSERT_NE(vdata.validity_, nullptr);
  EXPECT_FALSE(vdata.validity_->RowIsValid(vdata.sel_->GetIndex(2)));
  EXPECT_TRUE(vdata.validity_->RowIsValid(vdata.sel_->GetIndex(0)));
}

TEST_F(VectorNullTest, OrrifyDictionaryCarriesValidity) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {4});
  SelectionVector sel(3);
  sel.SetIndex(0, 0);
  sel.SetIndex(1, 4);  // null
  sel.SetIndex(2, 2);
  Vector dict(v, sel, 3);
  VectorData vdata;
  dict.Orrify(3, vdata);
  ASSERT_NE(vdata.validity_, nullptr);
  EXPECT_TRUE(vdata.validity_->RowIsValid(vdata.sel_->GetIndex(0)));
  EXPECT_FALSE(vdata.validity_->RowIsValid(vdata.sel_->GetIndex(1)));
  EXPECT_TRUE(vdata.validity_->RowIsValid(vdata.sel_->GetIndex(2)));
}

// ---------- SEQUENCE / SEQUENCE_CIRCULAR: always valid ----------

TEST_F(VectorNullTest, SequenceAlwaysValid) {
  Vector v(PhysicalType::BIGINT);
  v.Sequence(5, 2);
  for (idx_t i = 0; i < 10; i++) {
    EXPECT_FALSE(IsNull(v, i));
  }
}

TEST_F(VectorNullTest, CircularSequenceAlwaysValid) {
  Vector v(PhysicalType::BIGINT);
  v.Sequence(5, 0, 1, 10);
  for (idx_t i = 0; i < 10; i++) {
    EXPECT_FALSE(IsNull(v, i));
  }
}

// ---------- STRING null (the WriteNullFill string path) ----------

TEST_F(VectorNullTest, StringNull) {
  Vector v(PhysicalType::STRING, 4);
  v.SetValue(0, Value("a"));
  v.SetValue(1, Value::Null());
  v.SetValue(2, Value("c"));
  EXPECT_FALSE(IsNull(v, 0));
  EXPECT_TRUE(IsNull(v, 1));
  EXPECT_TRUE(v.GetValue(1).IsNull());
  EXPECT_EQ(v.GetValue(1).ToString(), "NULL");
  EXPECT_EQ(v.GetValue(2), Value("c"));
}

// ---------- CONSTANT vector get / SetNull ----------

TEST_F(VectorNullTest, ConstantNullGetValue) {
  Vector v(Value::Null(LogicalTypeId::INTEGER));
  ASSERT_EQ(v.GetVectorType(), VectorType::CONSTANT_VECTOR);
  EXPECT_TRUE(ConstantVector::IsNull(v));
  EXPECT_TRUE(v.GetValue(0).IsNull());
  // A constant null reports null for every logical row.
  EXPECT_TRUE(IsNull(v, 3));
}

TEST_F(VectorNullTest, ConstantSetNullToggles) {
  Vector v(Value(7));
  EXPECT_FALSE(ConstantVector::IsNull(v));
  EXPECT_EQ(v.GetValue(0), Value(7));
  ConstantVector::SetNull(v, true);
  EXPECT_TRUE(ConstantVector::IsNull(v));
  EXPECT_TRUE(v.GetValue(0).IsNull());
  ConstantVector::SetNull(v, false);
  EXPECT_FALSE(ConstantVector::IsNull(v));
}

// ---------- SetValue(NULL) dispatched through a DICTIONARY to the child ----------

TEST_F(VectorNullTest, SetNullOnDictionaryDispatchesToChild) {
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {});  // no nulls yet
  SelectionVector sel(3);
  sel.SetIndex(0, 4);
  sel.SetIndex(1, 1);
  sel.SetIndex(2, 2);
  Vector dict(v, sel, 3);
  ASSERT_EQ(dict.GetVectorType(), VectorType::DICTIONARY_VECTOR);
  // SetValue dispatches through the selection into the dictionary's child mask.
  dict.SetValue(0, Value::Null());  // dict row 0 -> child row 4
  EXPECT_TRUE(IsNull(dict, 0));
  EXPECT_FALSE(IsNull(dict, 1));
  EXPECT_TRUE(dict.GetValue(0).IsNull());
  EXPECT_FALSE(dict.GetValue(1).IsNull());
  // NOTE: when the source mask was all-valid (no buffer), the dictionary's child owns an
  // independent mask, so this does NOT retro-mark the original `v` — that is the intended
  // zero-overhead all-valid behavior, not cross-view propagation.
}

// ---------- Move ctor preserves nulls ----------

TEST_F(VectorNullTest, MoveCtorPreservesNull) {
  Vector a = CreateVectorWithNulls(PhysicalType::INTEGER, 5, {2});
  Vector b(std::move(a));
  EXPECT_TRUE(IsNull(b, 2));
  EXPECT_FALSE(IsNull(b, 0));
}

// ---------- Resize grows the mask, preserving nulls ----------

TEST_F(VectorNullTest, ResizeGrowsMaskPreservingNull) {
  Vector v(PhysicalType::INTEGER, 4);
  for (idx_t i = 0; i < 4; i++) {
    v.SetValue(i, Value(static_cast<int32_t>(i)));
  }
  v.SetValue(2, Value::Null());
  v.Resize(4, 2000);  // grows past one mask word block
  v.SetValue(1500, Value(123));
  EXPECT_TRUE(IsNull(v, 2));      // preserved
  EXPECT_FALSE(IsNull(v, 1500));  // the newly written row is valid
  EXPECT_EQ(v.GetValue(1500), Value(123));
}

// ---------- VectorOperations::Copy carries nulls from non-flat sources ----------

TEST_F(VectorNullTest, CopyFromDictionarySourceCarriesNull) {
  Vector src = CreateVectorWithNulls(PhysicalType::INTEGER, 6, {4});
  SelectionVector sel(3);
  sel.SetIndex(0, 0);
  sel.SetIndex(1, 4);  // null
  sel.SetIndex(2, 2);
  Vector dict_src(src, sel, 3);

  Vector target(PhysicalType::INTEGER, STANDARD_VECTOR_SIZE);
  VectorOperations::Copy(dict_src, target, 3, 0, 0);
  EXPECT_FALSE(IsNull(target, 0));
  EXPECT_TRUE(IsNull(target, 1));
  EXPECT_FALSE(IsNull(target, 2));
}

TEST_F(VectorNullTest, CopyFromConstantNullSourceCarriesNull) {
  Vector const_null(Value::Null(LogicalTypeId::INTEGER));
  Vector target(PhysicalType::INTEGER, STANDARD_VECTOR_SIZE);
  VectorOperations::Copy(const_null, target, 5, 0, 0);
  for (idx_t i = 0; i < 5; i++) {
    EXPECT_TRUE(IsNull(target, i));
  }
}

// ---------- Multi-batch (> 5000 rows) ----------

TEST_F(VectorNullTest, MultiBatchRandomNulls) {
  const idx_t count = 6000;
  auto positions = RandomNullPlacement(count, 0.1, 123);
  Vector v(PhysicalType::BIGINT, count);
  for (idx_t i = 0; i < count; i++) {
    v.SetValue(i, Value(static_cast<int64_t>(i)));
  }
  for (auto p : positions) {
    v.SetValue(p, Value::Null());
  }

  std::vector<bool> expected_null(count, false);
  for (auto p : positions) {
    expected_null[p] = true;
  }
  for (idx_t i = 0; i < count; i++) {
    EXPECT_EQ(IsNull(v, i), expected_null[i]) << "row " << i;
  }
}

}  // namespace bumblebee
