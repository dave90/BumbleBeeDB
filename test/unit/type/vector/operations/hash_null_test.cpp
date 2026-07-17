//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// hash_null_test.cpp
//
// Identification: test/unit/type/vector/operations/hash_null_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <vector>

#include "common/config.h"
#include "common/hash.h"
#include "gtest/gtest.h"
#include "null_test_base.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

// A NULL row must hash to a fixed sentinel rather than to whatever bit pattern happens to sit
// in its (logically absent) data slot: two NULL keys have to collide into the same hash
// bucket for a future hash-join / hash-aggregate to group them, and a NULL must not
// accidentally share a bucket with a real value that mirrors the stale bytes. The actual key
// equality is still resolved separately by the comparison ops, which treat NULL correctly.
class HashNullTest : public NullTestBase {
 protected:
  // The sentinel is internal to vector_hash.cpp, so obtain it observably: hashing a NULL
  // constant yields exactly the NULL-row hash.
  static auto NullHashSentinel(const LogicalType &type) -> hash_t {
    Vector null_const(Value::Null(type));
    Vector out(LogicalTypeId::HASH);
    VectorOperations::Hash(null_const, out, 1);
    EXPECT_EQ(out.GetVectorType(), VectorType::CONSTANT_VECTOR);
    return *ConstantVector::GetData<hash_t>(out);
  }

  static auto CombineScalar(hash_t a, hash_t b) -> hash_t { return (a * UINT64_C(0xbf58476d1ce4e5b9)) ^ b; }
};

// Every NULL row hashes to the same sentinel, and non-null rows hash to their value.
TEST_F(HashNullTest, NullRowsHashToStableSentinel) {
  const idx_t count = 6;
  Vector v = CreateVectorWithNulls(PhysicalType::INTEGER, count, {1, 4});  // rows hold 1..6, null at 1,4
  Vector hashes(LogicalTypeId::HASH, count);

  VectorOperations::Hash(v, hashes, count);
  auto *hd = FlatVector::GetData<hash_t>(hashes);
  hash_t sentinel = NullHashSentinel(PhysicalType::INTEGER);

  for (idx_t i = 0; i < count; i++) {
    if (i == 1 || i == 4) {
      EXPECT_EQ(hd[i], sentinel) << "null row " << i;
    } else {
      EXPECT_EQ(hd[i], Hash<int32_t>(static_cast<int32_t>(i + 1))) << "row " << i;
      EXPECT_NE(hd[i], sentinel) << "real value collided with the null sentinel at row " << i;
    }
  }
}

// The NULL sentinel does not depend on the column type: a NULL INTEGER and a NULL BIGINT key
// hash the same, so a NULL never fails to match another NULL across a widened join key.
TEST_F(HashNullTest, NullSentinelIsTypeIndependent) {
  EXPECT_EQ(NullHashSentinel(PhysicalType::INTEGER), NullHashSentinel(PhysicalType::BIGINT));
  EXPECT_EQ(NullHashSentinel(PhysicalType::INTEGER), NullHashSentinel(PhysicalType::STRING));
}

// A NULL constant vector hashes to the sentinel as a CONSTANT result.
TEST_F(HashNullTest, ConstantNullHashesToSentinel) {
  Vector null_const(Value::Null(PhysicalType::INTEGER));
  Vector hashes(LogicalTypeId::HASH);
  VectorOperations::Hash(null_const, hashes, 4);

  EXPECT_EQ(hashes.GetVectorType(), VectorType::CONSTANT_VECTOR);
  EXPECT_EQ(*ConstantVector::GetData<hash_t>(hashes), NullHashSentinel(PhysicalType::INTEGER));
}

// Hashing a NULL through a dictionary encoding must still see the null via the child mask.
TEST_F(HashNullTest, DictionaryNullHashesToSentinel) {
  Vector base = CreateVectorWithNulls(PhysicalType::INTEGER, 5, {2});  // null at physical row 2
  SelectionVector sel(3);
  sel.SetIndex(0, 0);  // 1
  sel.SetIndex(1, 2);  // NULL
  sel.SetIndex(2, 4);  // 5
  Vector dict(base, sel, 3);

  Vector hashes(LogicalTypeId::HASH, 3);
  VectorOperations::Hash(dict, hashes, 3);
  auto *hd = FlatVector::GetData<hash_t>(hashes);
  hash_t sentinel = NullHashSentinel(PhysicalType::INTEGER);

  EXPECT_EQ(hd[0], Hash<int32_t>(1));
  EXPECT_EQ(hd[1], sentinel);
  EXPECT_EQ(hd[2], Hash<int32_t>(5));
}

// CombineHash folds the NULL sentinel — not garbage — for a null row, so the combined hash of
// a composite key stays deterministic when one of its columns is NULL.
TEST_F(HashNullTest, CombineHashFoldsNullSentinel) {
  const idx_t count = 4;
  Vector base(PhysicalType::INTEGER, count);
  for (idx_t i = 0; i < count; i++) {
    base.SetValue(i, Value(static_cast<int32_t>(i) + 100));
  }
  Vector second = CreateVectorWithNulls(PhysicalType::INTEGER, count, {2});  // null at row 2

  Vector hashes(LogicalTypeId::HASH, count);
  VectorOperations::Hash(base, hashes, count);
  // Snapshot the first-column hashes before folding.
  std::vector<hash_t> first(count);
  auto *hd = FlatVector::GetData<hash_t>(hashes);
  for (idx_t i = 0; i < count; i++) {
    first[i] = hd[i];
  }

  VectorOperations::CombineHash(hashes, second, count);
  hash_t sentinel = NullHashSentinel(PhysicalType::INTEGER);
  for (idx_t i = 0; i < count; i++) {
    hash_t other = (i == 2) ? sentinel : Hash<int32_t>(static_cast<int32_t>(i + 1));
    EXPECT_EQ(hd[i], CombineScalar(first[i], other)) << "row " << i;
  }
}

// Two independently built columns with the same null layout produce identical hashes — the
// property a hash-join relies on to route equal keys to the same bucket.
TEST_F(HashNullTest, SameNullLayoutHashesIdentically) {
  const idx_t count = 5000;
  auto positions = RandomNullPlacement(count, 0.1, 7);

  Vector a(PhysicalType::BIGINT, count);
  Vector b(PhysicalType::BIGINT, count);
  for (idx_t i = 0; i < count; i++) {
    a.SetValue(i, Value(static_cast<int64_t>(i)));
    b.SetValue(i, Value(static_cast<int64_t>(i)));
  }
  for (auto p : positions) {
    a.SetValue(p, Value::Null(PhysicalType::BIGINT));
    b.SetValue(p, Value::Null(PhysicalType::BIGINT));
  }

  Vector ha(LogicalTypeId::HASH, count);
  Vector hb(LogicalTypeId::HASH, count);
  VectorOperations::Hash(a, ha, count);
  VectorOperations::Hash(b, hb, count);
  auto *pa = FlatVector::GetData<hash_t>(ha);
  auto *pb = FlatVector::GetData<hash_t>(hb);
  for (idx_t i = 0; i < count; i++) {
    EXPECT_EQ(pa[i], pb[i]) << "row " << i;
  }
}

}  // namespace bumblebee
