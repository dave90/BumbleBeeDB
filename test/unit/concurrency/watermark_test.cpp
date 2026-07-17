//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// watermark_test.cpp
//
// Identification: test/unit/concurrency/watermark_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "concurrency/watermark.h"

#include "gtest/gtest.h"

namespace bumblebee {

// With no live readers the watermark falls back to the latest commit ts.
TEST(WatermarkTest, EmptyFallsBackToCommitTs) {
  Watermark wm{10};
  EXPECT_EQ(wm.GetWatermark(), 10);
  wm.UpdateCommitTs(15);
  EXPECT_EQ(wm.GetWatermark(), 15);
}

// The watermark is the smallest live read ts.
TEST(WatermarkTest, TracksOldestLiveReadTs) {
  Watermark wm{5};
  wm.AddTxn(5);
  wm.AddTxn(7);
  wm.AddTxn(6);
  EXPECT_EQ(wm.GetWatermark(), 5);
  wm.RemoveTxn(5);
  EXPECT_EQ(wm.GetWatermark(), 6);
  wm.RemoveTxn(6);
  EXPECT_EQ(wm.GetWatermark(), 7);
}

// Reference counting: a duplicated read ts must be removed as many times as added.
TEST(WatermarkTest, RefCountsDuplicateReadTs) {
  Watermark wm{5};
  wm.AddTxn(5);
  wm.AddTxn(5);
  wm.AddTxn(8);
  EXPECT_EQ(wm.GetWatermark(), 5);
  wm.RemoveTxn(5);
  EXPECT_EQ(wm.GetWatermark(), 5) << "one live reader still at 5";
  wm.RemoveTxn(5);
  EXPECT_EQ(wm.GetWatermark(), 8) << "last 5 gone; oldest live is now 8";
}

// The audited bug: removing the last live reader must not deref begin() on an empty set. After the
// set empties, the watermark falls back to commit ts, and the tracker stays reusable.
TEST(WatermarkTest, RemoveLastReaderNoUnderflow) {
  Watermark wm{5};
  wm.AddTxn(5);
  wm.RemoveTxn(5);  // set now empty — must not UB
  EXPECT_EQ(wm.GetWatermark(), 5);
  wm.UpdateCommitTs(9);
  EXPECT_EQ(wm.GetWatermark(), 9);
  wm.AddTxn(9);
  EXPECT_EQ(wm.GetWatermark(), 9);
}

}  // namespace bumblebee
