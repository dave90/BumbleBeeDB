//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// validity_mask_test.cpp
//
// Identification: test/unit/type/vector/validity_mask_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/validity_mask.h"

#include "gtest/gtest.h"
#include "type/vector/selection_vector.h"

namespace bumblebee {

// A freshly constructed mask is all-valid with no backing buffer.
TEST(ValidityMaskTest, FreshMaskIsAllValid) {
  ValidityMask mask;
  EXPECT_TRUE(mask.AllValid());
  EXPECT_EQ(mask.Data(), nullptr);
  for (idx_t i = 0; i < 128; i++) {
    EXPECT_TRUE(mask.RowIsValid(i));
  }
}

// Setting a bit invalid clears it; RowIsValid reflects the change.
TEST(ValidityMaskTest, SetInvalidClearsBit) {
  ValidityMask mask;
  mask.SetInvalid(5);
  EXPECT_FALSE(mask.RowIsValid(5));
  EXPECT_TRUE(mask.RowIsValid(4));
  EXPECT_TRUE(mask.RowIsValid(6));
  // Bits across a word boundary.
  mask.SetInvalid(70);
  EXPECT_FALSE(mask.RowIsValid(70));
  EXPECT_TRUE(mask.RowIsValid(69));
}

// SetValid re-enables a previously cleared bit.
TEST(ValidityMaskTest, SetValidRestoresBit) {
  ValidityMask mask;
  mask.SetInvalid(9);
  EXPECT_FALSE(mask.RowIsValid(9));
  mask.SetValid(9);
  EXPECT_TRUE(mask.RowIsValid(9));
}

// EnsureWritable lazily allocates an otherwise all-ones buffer.
TEST(ValidityMaskTest, EnsureWritableLazyAllocAllOnes) {
  ValidityMask mask;
  EXPECT_EQ(mask.Data(), nullptr);
  mask.EnsureWritable();
  ASSERT_NE(mask.Data(), nullptr);
  // The buffer must be all-valid right after allocation.
  for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
    EXPECT_TRUE(mask.RowIsValid(i));
  }
}

// SetAllInvalid / SetAllValid toggle the whole mask.
TEST(ValidityMaskTest, SetAllInvalidThenAllValid) {
  ValidityMask mask;
  mask.SetAllInvalid(100);
  for (idx_t i = 0; i < 100; i++) {
    EXPECT_FALSE(mask.RowIsValid(i));
  }
  mask.SetAllValid();
  EXPECT_TRUE(mask.AllValid());
  for (idx_t i = 0; i < 100; i++) {
    EXPECT_TRUE(mask.RowIsValid(i));
  }
}

// CheckAllValid is true for a fresh mask, false after a single clear.
TEST(ValidityMaskTest, CheckAllValid) {
  ValidityMask mask;
  EXPECT_TRUE(mask.CheckAllValid(STANDARD_VECTOR_SIZE));
  mask.SetInvalid(3);
  EXPECT_FALSE(mask.CheckAllValid(10));
  // Still valid above index 3.
  EXPECT_TRUE(mask.RowIsValid(4));
}

// Slice copies bits at an offset, preserving relative positions.
TEST(ValidityMaskTest, SlicePreservesBitsAtOffset) {
  ValidityMask src;
  src.SetInvalid(5);
  src.SetInvalid(7);

  ValidityMask dst;
  dst.Slice(src, 4, 8);  // copy bits [4,12)
  // src bit 5 -> dst bit 1 ; src bit 7 -> dst bit 3
  EXPECT_TRUE(dst.RowIsValid(0));
  EXPECT_FALSE(dst.RowIsValid(1));
  EXPECT_TRUE(dst.RowIsValid(2));
  EXPECT_FALSE(dst.RowIsValid(3));
  EXPECT_TRUE(dst.RowIsValid(4));
}

// Slicing an all-valid mask yields an all-valid mask (no buffer).
TEST(ValidityMaskTest, SliceAllValidCollapses) {
  ValidityMask src;
  ValidityMask dst;
  dst.SetInvalid(0);  // dirty it first
  dst.Slice(src, 2, 8);
  EXPECT_TRUE(dst.AllValid());
}

// Combine is a logical AND: invalid in either input is invalid in the result.
TEST(ValidityMaskTest, CombineAnds) {
  ValidityMask a;
  a.SetInvalid(2);
  ValidityMask b;
  b.SetInvalid(5);
  a.Combine(b, 16);
  EXPECT_FALSE(a.RowIsValid(2));  // from a
  EXPECT_FALSE(a.RowIsValid(5));  // from b
  EXPECT_TRUE(a.RowIsValid(0));
  EXPECT_TRUE(a.RowIsValid(7));
}

// GatherFrom reads validity through a selection vector.
TEST(ValidityMaskTest, GatherThroughSelection) {
  ValidityMask src;
  src.SetInvalid(3);

  SelectionVector sel(4);
  sel.SetIndex(0, 0);
  sel.SetIndex(1, 3);  // null
  sel.SetIndex(2, 1);
  sel.SetIndex(3, 3);  // null

  ValidityMask dst;
  dst.GatherFrom(src, sel, 4);
  EXPECT_TRUE(dst.RowIsValid(0));
  EXPECT_FALSE(dst.RowIsValid(1));
  EXPECT_TRUE(dst.RowIsValid(2));
  EXPECT_FALSE(dst.RowIsValid(3));
}

// The copy constructor shares the underlying buffer (shallow) — referenced vectors share
// their validity, matching the data-sharing model.
TEST(ValidityMaskTest, CopyIsShallowShare) {
  ValidityMask a;
  a.SetInvalid(4);
  ValidityMask b(a);
  EXPECT_FALSE(b.RowIsValid(4));
  b.SetValid(4);
  EXPECT_TRUE(a.RowIsValid(4));  // shared buffer: the change is visible to both
}

// Copy() produces an independent deep copy.
TEST(ValidityMaskTest, ExplicitCopyIsDeep) {
  ValidityMask a;
  a.SetInvalid(4);
  ValidityMask b = a.Copy();
  EXPECT_FALSE(b.RowIsValid(4));
  b.SetValid(4);
  EXPECT_FALSE(a.RowIsValid(4));  // original untouched
  EXPECT_TRUE(b.RowIsValid(4));
}

// Copy() of an all-valid mask stays all-valid (no buffer).
TEST(ValidityMaskTest, CopyOfAllValidStaysAllValid) {
  ValidityMask a;
  ValidityMask b = a.Copy();
  EXPECT_TRUE(b.AllValid());
}

// Growing the buffer past its current size preserves already-written bits.
TEST(ValidityMaskTest, GrowPreservesBits) {
  ValidityMask m;
  m.SetInvalid(5);                           // first alloc: floored to STANDARD_VECTOR_SIZE
  m.SetInvalid(STANDARD_VECTOR_SIZE + 100);  // forces a grow + preserve
  EXPECT_FALSE(m.RowIsValid(5));             // low bit preserved across the grow
  EXPECT_FALSE(m.RowIsValid(STANDARD_VECTOR_SIZE + 100));
  EXPECT_TRUE(m.RowIsValid(6));
  EXPECT_TRUE(m.RowIsValid(STANDARD_VECTOR_SIZE + 50));
}

// Combine with an all-valid other is a no-op (early return).
TEST(ValidityMaskTest, CombineWithAllValidIsNoop) {
  ValidityMask a;
  a.SetInvalid(3);
  ValidityMask all_valid;
  a.Combine(all_valid, 16);
  EXPECT_FALSE(a.RowIsValid(3));  // unchanged
  EXPECT_TRUE(a.RowIsValid(0));
}

// GatherFrom an all-valid source leaves the destination all-valid.
TEST(ValidityMaskTest, GatherFromAllValidStaysValid) {
  ValidityMask src;
  SelectionVector sel(4);
  for (idx_t i = 0; i < 4; i++) {
    sel.SetIndex(i, i);
  }
  ValidityMask dst;
  dst.SetInvalid(0);  // dirty first
  dst.GatherFrom(src, sel, 4);
  EXPECT_TRUE(dst.AllValid());
}

// Slice with an offset past the source's covered range yields an all-valid result.
TEST(ValidityMaskTest, SliceOffsetPastSourceIsAllValid) {
  ValidityMask src;
  src.SetInvalid(2);  // src now covers STANDARD_VECTOR_SIZE bits
  ValidityMask dst;
  dst.Slice(src, STANDARD_VECTOR_SIZE + 10, 8);  // offset beyond the source bits
  for (idx_t i = 0; i < 8; i++) {
    EXPECT_TRUE(dst.RowIsValid(i));
  }
}

// DebugConsistent: all-valid (no buffer) is consistent; after SetInvalid the buffer is
// owned and large enough for the addressed row, including after a grow.
TEST(ValidityMaskTest, DebugConsistentInvariant) {
  ValidityMask m;
  EXPECT_TRUE(m.DebugConsistent(STANDARD_VECTOR_SIZE));  // no buffer => consistent
  m.SetInvalid(5);
  EXPECT_TRUE(m.DebugConsistent(STANDARD_VECTOR_SIZE));
  m.SetInvalid(5000);  // grows the buffer past one word
  EXPECT_TRUE(m.DebugConsistent(5001));
}

}  // namespace bumblebee
