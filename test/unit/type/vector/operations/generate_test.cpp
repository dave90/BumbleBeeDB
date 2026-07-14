//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// generate_test.cpp
//
// Identification: test/unit/type/vector/operations/generate_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/config.h"
#include "gtest/gtest.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

TEST(VectorOperationsGenerateSequenceTest, BasicSequenceGeneration) {
  const idx_t count = 5;
  Vector result(PhysicalType::INTEGER, count);
  VectorOperations::GenerateSequence(result, count, 10, 2);
  auto *data = FlatVector::GetData<int32_t>(result);
  for (idx_t i = 0; i < count; ++i) {
    ASSERT_EQ(data[i], 10 + static_cast<int32_t>(i) * 2);
  }
}

TEST(VectorOperationsGenerateSequenceTest, NegativeIncrementSequence) {
  const idx_t count = 4;
  Vector result(PhysicalType::INTEGER, count);
  VectorOperations::GenerateSequence(result, count, 20, -3);
  auto *data = FlatVector::GetData<int32_t>(result);
  for (idx_t i = 0; i < count; ++i) {
    ASSERT_EQ(data[i], 20 - static_cast<int32_t>(i) * 3);
  }
}

// Only the selected rows are written, and each holds the value the sequence has AT THAT
// ROW — not at the loop counter. So reading result[sel[i]] agrees with the full sequence.
TEST(VectorOperationsGenerateSequenceTest, SequenceWithSelectionVector) {
  const idx_t count = 3;
  Vector result(PhysicalType::INTEGER, 5);  // allocate more than is written
  SelectionVector sel(count);
  sel.SetIndex(0, 2);
  sel.SetIndex(1, 4);
  sel.SetIndex(2, 1);

  VectorOperations::GenerateSequence(result, count, sel, 100, 1);
  auto *data = FlatVector::GetData<int32_t>(result);

  ASSERT_EQ(data[1], 100 + 1);
  ASSERT_EQ(data[2], 100 + 2);
  ASSERT_EQ(data[4], 100 + 4);
}

TEST(VectorOperationsGenerateSequenceTest, BasicCircularSequenceGeneration) {
  const idx_t count = 100;
  Vector result(PhysicalType::INTEGER, count);
  // [10, 50], stride 1, offset 5: the 41 values repeat, shifted by 5.
  VectorOperations::GenerateSequence(result, count, 10, 5, 1, 50);
  auto *data = FlatVector::GetData<int32_t>(result);
  for (idx_t i = 0; i < count; ++i) {
    ASSERT_EQ(data[i], 10 + static_cast<int32_t>((i + 5) % 41));
  }
}

TEST(VectorOperationsGenerateSequenceTest, BasicCircularSelectionSequenceGeneration) {
  const idx_t count = 7;
  Vector result(PhysicalType::INTEGER);
  SelectionVector sel(count);
  sel.SetIndex(0, 0);
  sel.SetIndex(1, 2);
  sel.SetIndex(2, 4);
  sel.SetIndex(3, 6);
  sel.SetIndex(4, 8);
  sel.SetIndex(5, 10);
  sel.SetIndex(6, 12);

  // [1, 5], stride 2: each value repeats twice. The selection picks the even rows, so the
  // written rows step through the 5 values one at a time.
  VectorOperations::GenerateSequence(result, count, sel, 1, 0, 2, 5);
  auto *data = FlatVector::GetData<int32_t>(result);
  for (idx_t i = 0; i < count; ++i) {
    auto idx = sel.GetIndex(i);
    ASSERT_EQ(data[idx], 1 + static_cast<int32_t>(i % 5));
  }
}

}  // namespace bumblebee
