//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// copy_test.cpp
//
// Identification: test/unit/type/vector/operations/copy_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>

#include "common/config.h"
#include "gtest/gtest.h"
#include "type/value.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

class VectorOperationsCopyTest : public ::testing::Test {
 protected:
  static constexpr idx_t TEST_COUNT = 100;

  void SetUp() override {
    source_ = std::make_unique<Vector>(PhysicalType::INTEGER, TEST_COUNT);
    target_ = std::make_unique<Vector>(PhysicalType::INTEGER, TEST_COUNT);

    auto *data = FlatVector::GetData<int32_t>(*source_);
    for (idx_t i = 0; i < TEST_COUNT; ++i) {
      data[i] = static_cast<int32_t>(i) * 10;
    }
  }

  std::unique_ptr<Vector> source_;
  std::unique_ptr<Vector> target_;
};

TEST_F(VectorOperationsCopyTest, CopyFlatVector) {
  VectorOperations::Copy(*source_, *target_, TEST_COUNT, 0, 0);
  auto *data = FlatVector::GetData<int32_t>(*target_);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    ASSERT_EQ(data[i], static_cast<int32_t>(i) * 10);
  }
}

TEST_F(VectorOperationsCopyTest, CopyWithOffset) {
  const idx_t offset = 10;
  VectorOperations::Copy(*source_, *target_, TEST_COUNT, offset, 0);
  auto *data = FlatVector::GetData<int32_t>(*target_);
  for (idx_t i = 0; i < TEST_COUNT - offset; ++i) {
    ASSERT_EQ(data[i], static_cast<int32_t>(i + offset) * 10);
  }
}

TEST_F(VectorOperationsCopyTest, CopyWithSelectionVector) {
  SelectionVector sel(5);
  sel.SetIndex(0, 4);
  sel.SetIndex(1, 3);
  sel.SetIndex(2, 2);
  sel.SetIndex(3, 1);
  sel.SetIndex(4, 0);

  VectorOperations::Copy(*source_, *target_, sel, 5, 0, 0);

  auto *data = FlatVector::GetData<int32_t>(*target_);
  for (idx_t i = 0; i < 5; ++i) {
    ASSERT_EQ(data[i], static_cast<int32_t>(4 - i) * 10);
  }
}

TEST_F(VectorOperationsCopyTest, CopyToConstantVector) {
  Value nine_nine_nine(999);
  Vector constant_target(nine_nine_nine);

  VectorOperations::Copy(*source_, constant_target, FlatVector::INCREMENTAL_SELECTION_VECTOR, 1, 0, 0);
  constant_target.SetVectorType(VectorType::FLAT_VECTOR);
  auto *data = FlatVector::GetData<int32_t>(constant_target);
  ASSERT_EQ(data[0], 0);
}

TEST_F(VectorOperationsCopyTest, CopyFromConstantVector) {
  Value forty_two(42);
  Vector constant_source(forty_two);

  VectorOperations::Copy(constant_source, *target_, ConstantVector::ZERO_SELECTION_VECTOR, TEST_COUNT, 0, 0);

  auto *data = FlatVector::GetData<int32_t>(*target_);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    ASSERT_EQ(data[i], 42);
  }
}

TEST_F(VectorOperationsCopyTest, CopyFromSequenceVector) {
  Vector sequence(PhysicalType::INTEGER, TEST_COUNT);
  sequence.Sequence(5, 3);  // 5, 8, 11, 14, ...

  VectorOperations::Copy(sequence, *target_, TEST_COUNT, 0, 0);

  auto *data = FlatVector::GetData<int32_t>(*target_);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    ASSERT_EQ(data[i], 5 + static_cast<int32_t>(i) * 3);
  }
}

TEST_F(VectorOperationsCopyTest, CopyFromDictionaryVector) {
  Vector dict_vector(PhysicalType::INTEGER, TEST_COUNT);
  auto *data = FlatVector::GetData<int32_t>(dict_vector);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    data[i] = static_cast<int32_t>(i) + 100;
  }
  // Reverse the rows through the selection: 0 -> TEST_COUNT - 1, 1 -> TEST_COUNT - 2, ...
  SelectionVector dict_sel(TEST_COUNT);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    dict_sel.SetIndex(i, TEST_COUNT - 1 - i);
  }
  dict_vector.Slice(dict_sel, TEST_COUNT);

  VectorOperations::Copy(dict_vector, *target_, TEST_COUNT, 0, 0);
  ASSERT_EQ(target_->GetVectorType(), VectorType::FLAT_VECTOR);

  auto *target_data = FlatVector::GetData<int32_t>(*target_);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    ASSERT_EQ(target_data[i], static_cast<int32_t>(100 + (TEST_COUNT - 1 - i)));
  }
}

// ---------------------------------------------------------------------------
// NULL semantics.
//
// The contract: the target's rows [target_offset, target_offset + count) reflect the
// source's validity EXACTLY. Copy sets the valid bits as well as the invalid ones, so a
// target reused across batches cannot leak a stale invalid from an earlier batch.
//
// Fast path: when both source and target are all-valid, no validity write happens at all.
// ---------------------------------------------------------------------------

class CopyNullTest : public ::testing::Test {
 protected:
  static constexpr idx_t N = 64;
};

// The source's nulls must be mirrored exactly onto the target.
TEST_F(CopyNullTest, PropagatesNullsFromSource) {
  Vector source(PhysicalType::INTEGER, N);
  Vector target(PhysicalType::INTEGER, N);
  auto *sdata = FlatVector::GetData<int32_t>(source);
  for (idx_t i = 0; i < N; i++) {
    sdata[i] = static_cast<int32_t>(i);
  }
  source.SetInvalid(3);
  source.SetInvalid(17);
  source.SetInvalid(40);

  VectorOperations::Copy(source, target, N, 0, 0);

  for (idx_t i = 0; i < N; i++) {
    const bool expected_valid = (i != 3 && i != 17 && i != 40);
    EXPECT_EQ(target.RowIsValid(i), expected_valid) << "row " << i;
  }
}

// The source row is VALID where the target holds a STALE invalid bit from a previous batch.
// After the copy the target must report the row VALID: the staleness must not leak through.
TEST_F(CopyNullTest, ClearsStaleInvalidWhenSourceIsValid) {
  Vector source(PhysicalType::INTEGER, N);
  Vector target(PhysicalType::INTEGER, N);
  auto *sdata = FlatVector::GetData<int32_t>(source);
  auto *tdata = FlatVector::GetData<int32_t>(target);
  for (idx_t i = 0; i < N; i++) {
    sdata[i] = static_cast<int32_t>(i + 100);
    tdata[i] = 0;
  }
  // The source is fully valid. The target has stale invalid bits from a "previous batch".
  target.SetInvalid(5);
  target.SetInvalid(20);
  ASSERT_FALSE(target.RowIsValid(5));
  ASSERT_FALSE(target.RowIsValid(20));

  VectorOperations::Copy(source, target, N, 0, 0);

  for (idx_t i = 0; i < N; i++) {
    EXPECT_TRUE(target.RowIsValid(i)) << "row " << i << " should be valid after the copy";
  }
}

// Mixed: the source is null at some rows, the target stale-invalid at others. Only the
// source's nulls may survive.
TEST_F(CopyNullTest, OverwritesTargetMaskExactlyFromSource) {
  Vector source(PhysicalType::INTEGER, N);
  Vector target(PhysicalType::INTEGER, N);
  auto *sdata = FlatVector::GetData<int32_t>(source);
  for (idx_t i = 0; i < N; i++) {
    sdata[i] = static_cast<int32_t>(i);
  }
  source.SetInvalid(10);
  source.SetInvalid(30);
  // Stale invalids on the target, at different rows than the source's.
  target.SetInvalid(5);
  target.SetInvalid(50);

  VectorOperations::Copy(source, target, N, 0, 0);

  for (idx_t i = 0; i < N; i++) {
    const bool expected_valid = (i != 10 && i != 30);
    EXPECT_EQ(target.RowIsValid(i), expected_valid) << "row " << i;
  }
}

// Both sides all-valid: the copy must not allocate a validity buffer on either.
TEST_F(CopyNullTest, AllValidFastPathDoesNotAllocate) {
  Vector source(PhysicalType::INTEGER, N);
  Vector target(PhysicalType::INTEGER, N);
  auto *sdata = FlatVector::GetData<int32_t>(source);
  for (idx_t i = 0; i < N; i++) {
    sdata[i] = static_cast<int32_t>(i);
  }

  ASSERT_TRUE(source.Validity().AllValid());
  ASSERT_TRUE(target.Validity().AllValid());

  VectorOperations::Copy(source, target, N, 0, 0);

  EXPECT_TRUE(target.Validity().AllValid());
}

// An append-style copy (target_offset > 0) must leave the target's earlier rows alone.
TEST_F(CopyNullTest, AppendStyleCopyKeepsEarlierTargetValidity) {
  Vector source(PhysicalType::INTEGER, N);
  Vector target(PhysicalType::INTEGER, 2 * N);
  auto *sdata = FlatVector::GetData<int32_t>(source);
  auto *tdata = FlatVector::GetData<int32_t>(target);
  for (idx_t i = 0; i < N; i++) {
    sdata[i] = static_cast<int32_t>(i);
  }
  for (idx_t i = 0; i < 2 * N; i++) {
    tdata[i] = -1;
  }
  source.SetInvalid(7);

  // An invalid row in the EARLIER region, which the append must not disturb.
  target.SetInvalid(2);

  VectorOperations::Copy(source, target, N, 0, N);

  // The earlier region is untouched.
  EXPECT_FALSE(target.RowIsValid(2));
  EXPECT_TRUE(target.RowIsValid(0));
  // The appended region reflects the source: N + 7 invalid, the rest valid.
  for (idx_t i = 0; i < N; i++) {
    const bool expected_valid = (i != 7);
    EXPECT_EQ(target.RowIsValid(N + i), expected_valid) << "row " << (N + i);
  }
}

// A DICTIONARY source keeps its validity on the child, indexed through the selection. The
// copy has to walk the selection to land the right bits on the target's flat rows.
TEST_F(CopyNullTest, DictionarySourceCarriesNullThroughSelection) {
  Vector child_source(PhysicalType::INTEGER, 8);
  auto *cdata = FlatVector::GetData<int32_t>(child_source);
  for (idx_t i = 0; i < 8; i++) {
    cdata[i] = static_cast<int32_t>(i * 10);
  }
  child_source.SetInvalid(2);
  child_source.SetInvalid(5);

  // The selection picks child rows [5, 1, 2, 7], so the target should be NULL at output
  // rows 0 (= child[5]) and 2 (= child[2]).
  Vector dict_source(child_source);
  SelectionVector sel(4);
  sel.SetIndex(0, 5);
  sel.SetIndex(1, 1);
  sel.SetIndex(2, 2);
  sel.SetIndex(3, 7);
  dict_source.Slice(sel, 4);

  Vector target(PhysicalType::INTEGER, 4);
  VectorOperations::Copy(dict_source, target, 4, 0, 0);

  EXPECT_FALSE(target.RowIsValid(0));  // from child[5]
  EXPECT_TRUE(target.RowIsValid(1));   // from child[1]
  EXPECT_FALSE(target.RowIsValid(2));  // from child[2]
  EXPECT_TRUE(target.RowIsValid(3));   // from child[7]
}

// A CONSTANT-NULL source has a single validity bit at row 0, broadcast through a zero
// selection. Every target row must end up NULL.
TEST_F(CopyNullTest, ConstantNullSourceMarksAllTargetRowsInvalid) {
  Value null_val = Value::Null(PhysicalType::INTEGER);
  Vector const_source(null_val);
  ASSERT_EQ(const_source.GetVectorType(), VectorType::CONSTANT_VECTOR);

  Vector target(PhysicalType::INTEGER, N);
  auto *tdata = FlatVector::GetData<int32_t>(target);
  for (idx_t i = 0; i < N; i++) {
    tdata[i] = 0;
  }

  VectorOperations::Copy(const_source, target, N, 0, 0);

  for (idx_t i = 0; i < N; i++) {
    EXPECT_FALSE(target.RowIsValid(i)) << "row " << i << " should be NULL";
  }
}

// The selection overload walks an explicit selection on the source: only the nulls at the
// SELECTED rows may land in the target.
TEST_F(CopyNullTest, SelectionOverloadOnlyCarriesSelectedNulls) {
  Vector source(PhysicalType::INTEGER, N);
  Vector target(PhysicalType::INTEGER, N);
  auto *sdata = FlatVector::GetData<int32_t>(source);
  for (idx_t i = 0; i < N; i++) {
    sdata[i] = static_cast<int32_t>(i);
  }
  source.SetInvalid(5);   // selected below
  source.SetInvalid(50);  // NOT selected: it must not surface in the target

  SelectionVector sel(3);
  sel.SetIndex(0, 5);
  sel.SetIndex(1, 6);
  sel.SetIndex(2, 7);
  VectorOperations::Copy(source, target, sel, 3, 0, 0);

  EXPECT_FALSE(target.RowIsValid(0));  // from source[5]
  EXPECT_TRUE(target.RowIsValid(1));   // from source[6]
  EXPECT_TRUE(target.RowIsValid(2));   // from source[7]
}

// Copying batch after batch into one reused target: no batch's invalid bits may leak into
// the region of another.
TEST_F(CopyNullTest, MultiBatchAppendDoesNotLeakStaleInvalidsAcrossBatches) {
  const idx_t batch_size = 64;
  const idx_t total_batches = 4;
  Vector target(PhysicalType::INTEGER, batch_size * total_batches);

  for (idx_t b = 0; b < total_batches; b++) {
    Vector batch_source(PhysicalType::INTEGER, batch_size);
    auto *sdata = FlatVector::GetData<int32_t>(batch_source);
    for (idx_t i = 0; i < batch_size; i++) {
      sdata[i] = static_cast<int32_t>(b * batch_size + i);
    }

    // Each odd batch carries a NULL at row 30; the even batches are fully valid. Without
    // the bidirectional validity write, the even batches' row 30 would inherit the odd
    // batches' invalid bit.
    if (b % 2 == 1) {
      batch_source.SetInvalid(30);
    }

    VectorOperations::Copy(batch_source, target, batch_size, 0, b * batch_size);
  }

  for (idx_t b = 0; b < total_batches; b++) {
    for (idx_t i = 0; i < batch_size; i++) {
      const idx_t pos = b * batch_size + i;
      const bool expected_valid = !(b % 2 == 1 && i == 30);
      EXPECT_EQ(target.RowIsValid(pos), expected_valid) << "batch " << b << " row " << i << " (pos " << pos << ")";
    }
  }
}

}  // namespace bumblebee
