//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// hash_test.cpp
//
// Identification: test/unit/type/vector/operations/hash_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <vector>

#include "common/config.h"
#include "common/hash.h"
#include "gtest/gtest.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

class VectorOperationsHashTest : public ::testing::Test {
 protected:
  static constexpr idx_t TEST_COUNT = 4;

  void SetUp() override {
    input_ = std::make_unique<Vector>(PhysicalType::INTEGER, TEST_COUNT);
    hashes_ = std::make_unique<Vector>(PhysicalType::UBIGINT, TEST_COUNT);

    auto *data = FlatVector::GetData<int32_t>(*input_);
    data[0] = 42;
    data[1] = 17;
    data[2] = -10;
    data[3] = -23;
  }

  std::unique_ptr<Vector> input_;
  std::unique_ptr<Vector> hashes_;
};

TEST_F(VectorOperationsHashTest, HashBasicFlatVector) {
  VectorOperations::Hash(*input_, *hashes_, TEST_COUNT);
  auto *hash_data = FlatVector::GetData<hash_t>(*hashes_);

  auto *input_data = FlatVector::GetData<int32_t>(*input_);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    ASSERT_EQ(hash_data[i], bumblebee::Hash<int32_t>(input_data[i]));
  }
}

TEST_F(VectorOperationsHashTest, HashWithSelectionVector) {
  SelectionVector sel(TEST_COUNT);
  sel.SetIndex(0, 3);
  sel.SetIndex(1, 1);
  sel.SetIndex(2, 2);

  VectorOperations::Hash(*input_, *hashes_, sel, 3);
  auto *hash_data = FlatVector::GetData<hash_t>(*hashes_);
  auto *input_data = FlatVector::GetData<int32_t>(*input_);

  // The hash of a selected row lands at that row's OWN index, not at the loop counter.
  for (idx_t i = 0; i < 3; ++i) {
    idx_t idx = sel.GetIndex(i);
    EXPECT_EQ(hash_data[idx], bumblebee::Hash<int32_t>(input_data[idx]));
  }
}

TEST_F(VectorOperationsHashTest, CombineHashWithFlatVectors) {
  Vector extra(PhysicalType::INTEGER, TEST_COUNT);
  auto *extra_data = FlatVector::GetData<int32_t>(extra);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    extra_data[i] = static_cast<int32_t>(i) + 100;
  }

  VectorOperations::Hash(*input_, *hashes_, TEST_COUNT);
  VectorOperations::CombineHash(*hashes_, extra, TEST_COUNT);

  auto *hash_data = FlatVector::GetData<hash_t>(*hashes_);
  auto *input_data = FlatVector::GetData<int32_t>(*input_);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    auto h1 = bumblebee::Hash<int32_t>(input_data[i]);
    auto h2 = bumblebee::Hash<int32_t>(extra_data[i]);
    ASSERT_EQ(hash_data[i], (h1 * UINT64_C(0xbf58476d1ce4e5b9)) ^ h2);
  }
}

TEST(VectorOperationsHashConstantTest, HashConstantVector) {
  Value val(123);
  Vector constant_input(val);
  Vector hash_result(LogicalTypeId::HASH);

  VectorOperations::Hash(constant_input, hash_result, 1);

  auto input_val = *ConstantVector::GetData<int32_t>(constant_input);
  auto hash_val = *ConstantVector::GetData<hash_t>(hash_result);
  EXPECT_EQ(hash_result.GetVectorType(), VectorType::CONSTANT_VECTOR);

  ASSERT_EQ(hash_val, bumblebee::Hash<int32_t>(input_val));
}

// Hashing a CONSTANT column leaves the accumulator CONSTANT. Folding a non-constant column
// into it then has to flatten the accumulator and broadcast the single hash across the rows.
// This is exactly what DataChunk::Hash does when its first column is a constant, so the path
// is live — and it reads the constant out before overwriting the slot it lives in.
TEST(VectorOperationsHashConstantTest, CombineHashFlattensAConstantAccumulator) {
  const idx_t count = 4;

  Value seven(7);
  Vector constant_col(seven);
  Vector flat_col(PhysicalType::INTEGER, count);
  auto *flat_data = FlatVector::GetData<int32_t>(flat_col);
  for (idx_t i = 0; i < count; ++i) {
    flat_data[i] = static_cast<int32_t>(i) + 100;
  }

  Vector hashes(LogicalTypeId::HASH);
  VectorOperations::Hash(constant_col, hashes, count);
  ASSERT_EQ(hashes.GetVectorType(), VectorType::CONSTANT_VECTOR);

  VectorOperations::CombineHash(hashes, flat_col, count);
  EXPECT_EQ(hashes.GetVectorType(), VectorType::FLAT_VECTOR);

  auto *hash_data = FlatVector::GetData<hash_t>(hashes);
  auto constant_hash = bumblebee::Hash<int32_t>(7);
  for (idx_t i = 0; i < count; ++i) {
    auto expected = (constant_hash * UINT64_C(0xbf58476d1ce4e5b9)) ^ bumblebee::Hash<int32_t>(flat_data[i]);
    EXPECT_EQ(hash_data[i], expected) << "row " << i;
  }
}

// STRING columns route through a distinct kernel from the fixed-width types; hash them and
// check each row against the scalar string hash.
TEST(VectorOperationsHashStringTest, HashStringVector) {
  const idx_t count = 4;
  Vector v(LogicalType(LogicalTypeId::STRING), count);
  v.SetValue(0, Value(std::string("")));
  v.SetValue(1, Value(std::string("bee")));
  v.SetValue(2, Value(std::string("a much longer string that will not fit inline")));
  v.SetValue(3, Value(std::string("bumble")));

  Vector hashes(LogicalTypeId::HASH, count);
  VectorOperations::Hash(v, hashes, count);

  auto *hd = FlatVector::GetData<hash_t>(hashes);
  auto *sd = FlatVector::GetData<string_t>(v);
  for (idx_t i = 0; i < count; i++) {
    EXPECT_EQ(hd[i], bumblebee::Hash<string_t>(sd[i])) << "row " << i;
  }
}

// A DICTIONARY-encoded input is orrified inside the kernel; its hashes must equal the hashes
// of the same logical values laid out flat — otherwise a dictionary probe would miss.
TEST(VectorOperationsHashDictionaryTest, HashDictionaryInputAgreesWithFlat) {
  Vector base(PhysicalType::INTEGER, 6);
  for (idx_t i = 0; i < 6; i++) {
    base.SetValue(i, Value(static_cast<int32_t>(i) * 10));
  }
  SelectionVector sel(3);
  sel.SetIndex(0, 5);  // 50
  sel.SetIndex(1, 1);  // 10
  sel.SetIndex(2, 3);  // 30
  Vector dict(base, sel, 3);

  Vector dict_hashes(LogicalTypeId::HASH, 3);
  VectorOperations::Hash(dict, dict_hashes, 3);
  auto *dh = FlatVector::GetData<hash_t>(dict_hashes);

  EXPECT_EQ(dh[0], bumblebee::Hash<int32_t>(50));
  EXPECT_EQ(dh[1], bumblebee::Hash<int32_t>(10));
  EXPECT_EQ(dh[2], bumblebee::Hash<int32_t>(30));
}

// The rsel overload of CombineHash folds only the selected rows, writing the result at each
// row's OWN index — the untouched rows must keep their prior accumulated hash.
TEST_F(VectorOperationsHashTest, CombineHashWithSelection) {
  Vector extra(PhysicalType::INTEGER, TEST_COUNT);
  auto *extra_data = FlatVector::GetData<int32_t>(extra);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    extra_data[i] = static_cast<int32_t>(i) + 100;
  }

  VectorOperations::Hash(*input_, *hashes_, TEST_COUNT);
  auto *hash_data = FlatVector::GetData<hash_t>(*hashes_);
  std::vector<hash_t> before(TEST_COUNT);
  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    before[i] = hash_data[i];
  }

  // Fold only rows 3 and 1.
  SelectionVector sel(2);
  sel.SetIndex(0, 3);
  sel.SetIndex(1, 1);
  VectorOperations::CombineHash(*hashes_, extra, sel, 2);

  for (idx_t i = 0; i < TEST_COUNT; ++i) {
    if (i == 3 || i == 1) {
      auto expected = (before[i] * UINT64_C(0xbf58476d1ce4e5b9)) ^ bumblebee::Hash<int32_t>(extra_data[i]);
      EXPECT_EQ(hash_data[i], expected) << "selected row " << i;
    } else {
      EXPECT_EQ(hash_data[i], before[i]) << "untouched row " << i;
    }
  }
}

// The same numeric value must hash the same whatever integer type it is stored in —
// otherwise a join between an INTEGER and a BIGINT key column would never match.
TEST(VectorOperationsHashConstantTest, HashNumericDifferentType) {
  Value val(123);
  Vector constant_input(val);
  Vector expected_hash_result(LogicalTypeId::HASH);

  VectorOperations::Hash(constant_input, expected_hash_result, 1);

  auto expected_hash_val = *ConstantVector::GetData<hash_t>(expected_hash_result);

  std::vector<PhysicalType> types = {PhysicalType::SMALLINT, PhysicalType::INTEGER, PhysicalType::USMALLINT,
                                     PhysicalType::UINTEGER, PhysicalType::UBIGINT};
  for (auto type : types) {
    Value cast_val = val.CastAs(type);
    Vector vec(cast_val);
    EXPECT_EQ(vec.GetType(), type);
    Vector hash_result(LogicalTypeId::HASH);
    VectorOperations::Hash(vec, hash_result, 1);
    auto hash_val = *ConstantVector::GetData<hash_t>(hash_result);
    EXPECT_EQ(expected_hash_val, hash_val);
  }
}

}  // namespace bumblebee
