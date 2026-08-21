//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// buffer_pool_manager_test.cpp
//
// Identification: test/unit/storage/buffer/buffer_pool_manager_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/buffer/buffer_pool_manager.h"

#include <cstring>
#include <memory>
#include <vector>

#include "common/config.h"
#include "gtest/gtest.h"
#include "storage/disk/disk_manager.h"
#include "storage/disk/memory_disk_manager.h"

namespace bumblebee {

static void WriteMarker(BufferPoolManager &bpm, page_id_t page_id, char marker) {
  auto guard = bpm.WritePage(page_id);
  std::memset(guard.GetDataMut(), marker, PAGE_SIZE);
}

static auto ReadMarker(BufferPoolManager &bpm, page_id_t page_id) -> char {
  auto guard = bpm.ReadPage(page_id);
  return guard.GetData()[0];
}

TEST(BufferPoolManagerTest, NewPageWriteReadRoundTrip) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(4, &dm);

  auto page_id = bpm.NewPage();
  WriteMarker(bpm, page_id, 'A');
  EXPECT_EQ(ReadMarker(bpm, page_id), 'A');
  EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{0});
}

TEST(BufferPoolManagerTest, PinCountTracksGuards) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(4, &dm);
  auto page_id = bpm.NewPage();
  {
    auto g1 = bpm.ReadPage(page_id);
    EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{1});
    {
      auto g2 = bpm.ReadPage(page_id);
      EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{2});
    }
    EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{1});
  }
  EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{0});
}

TEST(BufferPoolManagerTest, TryWritePageDoesNotWaitOrLeakAPin) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(4, &dm);
  auto page_id = bpm.NewPage();

  auto held = bpm.WritePage(page_id);
  EXPECT_FALSE(bpm.TryWritePage(page_id).has_value());
  EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{1});

  held.Drop();
  auto acquired = bpm.TryWritePage(page_id);
  ASSERT_TRUE(acquired.has_value());
  EXPECT_EQ(bpm.GetPinCount(page_id), std::optional<size_t>{1});
}

// Bug #2a: a dirty page evicted under memory pressure must be written back (awaited) before its
// frame is recycled — so reading it back later returns the written contents.
TEST(BufferPoolManagerTest, DirtyEvictionThenRereadSurvives) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(2, &dm);  // only 2 frames

  auto p0 = bpm.NewPage();
  auto p1 = bpm.NewPage();
  auto p2 = bpm.NewPage();

  WriteMarker(bpm, p0, '0');
  WriteMarker(bpm, p1, '1');
  // Touch two more pages, forcing p0 (and then p1) out of the two-frame pool.
  WriteMarker(bpm, p2, '2');
  {
    auto g = bpm.ReadPage(p1);
  }  // ensure p1 stays / reloads

  // p0 was dirty and evicted; its data must have survived to disk.
  EXPECT_EQ(ReadMarker(bpm, p0), '0');
  EXPECT_EQ(ReadMarker(bpm, p2), '2');
}

/** @brief A disk manager whose writes always fail, to exercise the eviction failure path. */
class WriteFailingDiskManager : public DiskManager {
 public:
  auto WritePage(page_id_t /*page_id*/, const_data_ptr_t /*page_data*/) -> bool override { return false; }
  auto ReadPage(page_id_t /*page_id*/, data_ptr_t page_data) -> bool override {
    std::memset(page_data, 0, PAGE_SIZE);
    return true;
  }
  auto DeletePage(page_id_t /*page_id*/) -> void override {}
};

// Bug #2b: when the dirty write-back fails, the frame must NOT be silently recycled (which would lose
// the page). The pool reports out-of-memory instead, and the dirty page is still readable in memory.
TEST(BufferPoolManagerTest, FailedWriteBackDoesNotRecycleFrame) {
  WriteFailingDiskManager dm;
  BufferPoolManager bpm(1, &dm);  // a single frame

  auto p0 = bpm.NewPage();
  {
    auto g = bpm.WritePage(p0);
    std::memset(g.GetDataMut(), 'X', PAGE_SIZE);
  }  // p0 dirty, now unpinned and evictable

  auto p1 = bpm.NewPage();
  // Bringing in p1 needs to evict p0, whose write-back fails: the pool must report OOM, not lose p0.
  auto guard = bpm.CheckedWritePage(p1);
  EXPECT_FALSE(guard.has_value()) << "eviction with a failing write-back must not free the frame";

  // p0 is still resident and intact.
  EXPECT_EQ(ReadMarker(bpm, p0), 'X');
}

TEST(BufferPoolManagerTest, DeletePinnedPageFails) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(4, &dm);
  auto page_id = bpm.NewPage();
  auto guard = bpm.WritePage(page_id);
  EXPECT_FALSE(bpm.DeletePage(page_id));  // pinned
}

}  // namespace bumblebee
