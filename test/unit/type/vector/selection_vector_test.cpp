//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// selection_vector_test.cpp
//
// Identification: test/unit/type/vector/selection_vector_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/selection_vector.h"

#include "gtest/gtest.h"

namespace bumblebee {

TEST(SelectionVectorTest, DefaultConstructor) {
  SelectionVector sv;
  EXPECT_EQ(sv.GetData(), nullptr);
  EXPECT_EQ(sv.GetIndex(5), 5);  // fallback if nullptr
  EXPECT_EQ(sv.ToString(), "");
}

TEST(SelectionVectorTest, ConstructorWithCount) {
  idx_t count = 10;
  SelectionVector sv(count);

  ASSERT_NE(sv.GetData(), nullptr);
  for (idx_t i = 0; i < count; ++i) {
    sv.SetIndex(i, i * 2);
  }
  for (idx_t i = 0; i < count; ++i) {
    EXPECT_EQ(sv.GetIndex(i), i * 2);
  }
}

TEST(SelectionVectorTest, ConstructorWithStartAndCount) {
  idx_t start = 100;
  idx_t count = 5;
  SelectionVector sv(start, count);

  for (idx_t i = 0; i < count; ++i) {
    EXPECT_EQ(sv.GetIndex(i), start + i);
  }
}

TEST(SelectionVectorTest, ConstructorWithRawPointer) {
  sel_t buffer[5] = {3, 1, 4, 1, 5};
  SelectionVector sv(buffer);

  for (idx_t i = 0; i < 5; ++i) {
    EXPECT_EQ(sv.GetIndex(i), buffer[i]);
  }
}

TEST(SelectionVectorTest, ConstructorWithSharedPointer) {
  sel_ptr_t ptr(new sel_t[3]{7, 8, 9});
  SelectionVector sv(ptr);

  EXPECT_EQ(sv.GetSelData(), ptr);
}

TEST(SelectionVectorTest, CopyConstructor) {
  SelectionVector original(3);
  original.SetIndex(0, 10);
  original.SetIndex(1, 20);
  original.SetIndex(2, 30);

  SelectionVector copy(original);
  for (idx_t i = 0; i < 3; ++i) {
    EXPECT_EQ(copy.GetIndex(i), original.GetIndex(i));
  }

  // The buffer is shared (it is held by a shared_ptr).
  copy.SetIndex(1, 99);
  EXPECT_EQ(original.GetIndex(1), 99);
}

TEST(SelectionVectorTest, SwapFunction) {
  SelectionVector sv(2);
  sv.SetIndex(0, 1);
  sv.SetIndex(1, 2);

  sv.Swap(0, 1);

  EXPECT_EQ(sv.GetIndex(0), 2);
  EXPECT_EQ(sv.GetIndex(1), 1);
}

TEST(SelectionVectorTest, SliceFunction) {
  SelectionVector base(5);
  for (idx_t i = 0; i < 5; ++i) {
    base.SetIndex(i, i * 10);
  }

  SelectionVector selector(3);
  selector.SetIndex(0, 1);
  selector.SetIndex(1, 3);
  selector.SetIndex(2, 4);

  auto sliced = base.Slice(selector, 3);
  EXPECT_EQ(sliced[0], 10);  // base[1]
  EXPECT_EQ(sliced[1], 30);  // base[3]
  EXPECT_EQ(sliced[2], 40);  // base[4]
}

TEST(SelectionVectorTest, OperatorSquareBrackets) {
  SelectionVector sv(2);
  sv[0] = 100;
  sv[1] = 200;

  EXPECT_EQ(sv.GetIndex(0), 100);
  EXPECT_EQ(sv.GetIndex(1), 200);
}

// Slicing a slice must COMPOSE the two selections, not nest them: the second selection
// indexes into the first's logical rows. This is the invariant the vectorized engine relies
// on when it slices an already-sliced column.
TEST(SelectionVectorTest, SliceComposesTwoLevels) {
  SelectionVector base(6);
  for (idx_t i = 0; i < 6; ++i) {
    base.SetIndex(i, i * 10);  // {0, 10, 20, 30, 40, 50}
  }

  SelectionVector sel1(4);
  sel1.SetIndex(0, 5);
  sel1.SetIndex(1, 2);
  sel1.SetIndex(2, 4);
  sel1.SetIndex(3, 1);
  SelectionVector level1(base.Slice(sel1, 4));  // {50, 20, 40, 10}

  SelectionVector sel2(2);
  sel2.SetIndex(0, 2);
  sel2.SetIndex(1, 0);
  SelectionVector level2(level1.Slice(sel2, 2));  // {40, 50}

  EXPECT_EQ(level2.GetIndex(0), 40);
  EXPECT_EQ(level2.GetIndex(1), 50);
  // The composed index equals base looked up through the full sel1 ∘ sel2 path.
  EXPECT_EQ(level2.GetIndex(0), base.GetIndex(sel1.GetIndex(sel2.GetIndex(0))));
  EXPECT_EQ(level2.GetIndex(1), base.GetIndex(sel1.GetIndex(sel2.GetIndex(1))));
}

// Initialize(count) on an existing selection replaces its buffer with a fresh one.
TEST(SelectionVectorTest, InitializeReallocates) {
  SelectionVector sv(3);
  sv.SetIndex(0, 7);

  sv.Initialize(static_cast<idx_t>(5));
  ASSERT_NE(sv.GetData(), nullptr);
  for (idx_t i = 0; i < 5; ++i) {
    sv.SetIndex(i, i + 100);
  }
  for (idx_t i = 0; i < 5; ++i) {
    EXPECT_EQ(sv.GetIndex(i), i + 100);
  }
}

}  // namespace bumblebee
