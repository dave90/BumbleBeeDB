//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bpm_concurrent_test.cpp
//
// Identification: test/unit/storage/buffer/bpm_concurrent_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <chrono>  // NOLINT
#include <cstring>
#include <thread>  // NOLINT
#include <vector>

#include "common/config.h"
#include "concurrency_test_util.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"

namespace bumblebee {

// Many threads hammer the write latch of the SAME page. The page latch must serialize them with no
// race / deadlock (TSan-clean), and the final content is one thread's complete write.
TEST(BpmConcurrentTest, WriteContentionSamePage) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(8, &dm);
  auto page_id = bpm.NewPage();

  const int threads = 4;
  const int rounds = 2000;
  LaunchParallelTest(threads, [&](uint64_t tid) {
    for (int r = 0; r < rounds; r++) {
      auto guard = bpm.WritePage(page_id);
      std::memset(guard.GetDataMut(), static_cast<char>('A' + tid), PAGE_SIZE);
    }
  });

  auto guard = bpm.ReadPage(page_id);
  // Every byte equals a single writer's fill (the last writer under the latch) — no torn page.
  const_data_ptr_t data = guard.GetData();
  char first = data[0];
  for (int i = 0; i < PAGE_SIZE; i++) {
    ASSERT_EQ(data[i], first);
  }
}

// Two threads each grab two page write latches in a CONSISTENT order (p0 then p1). A caller that
// orders page latches consistently must never deadlock — this verifies the BPM's *internal* latching
// (the pool latch dropped before the frame latch) introduces no lock-order cycle of its own.
// (Crossed page-latch acquisition would deadlock for any per-page-latch pool; ordering is the
// caller's responsibility, not the BPM's, so it is deliberately not tested here.)
TEST(BpmConcurrentTest, NoDeadlockConsistentOrder) {
  MemoryDiskManager dm(64);
  BufferPoolManager bpm(8, &dm);
  auto p0 = bpm.NewPage();
  auto p1 = bpm.NewPage();

  LaunchParallelTest(2, [&](uint64_t tid) {
    for (int r = 0; r < 1000; r++) {
      auto a = bpm.WritePage(p0);
      std::this_thread::yield();
      auto b = bpm.WritePage(p1);
      std::memset(a.GetDataMut(), static_cast<char>('0' + tid), 8);
      std::memset(b.GetDataMut(), static_cast<char>('0' + tid), 8);
    }
  });
  SUCCEED();  // reaching here without hanging is the assertion
}

// Concurrent fetch/evict pressure: more distinct pages than frames, many readers — no crash / race.
TEST(BpmConcurrentTest, ConcurrentFetchUnderEviction) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(4, &dm);  // only 4 frames
  std::vector<page_id_t> pages;
  for (int i = 0; i < 64; i++) {
    auto pid = bpm.NewPage();
    auto g = bpm.WritePage(pid);
    std::memset(g.GetDataMut(), static_cast<char>(i), PAGE_SIZE);
    pages.push_back(pid);
  }

  LaunchParallelTest(4, [&](uint64_t /*tid*/) {
    for (int rep = 0; rep < 200; rep++) {
      for (auto pid : pages) {
        auto g = bpm.ReadPage(pid);
        volatile char c = g.GetData()[0];
        (void)c;
      }
    }
  });
  SUCCEED();
}

// Many concurrent readers over more pages than frames: each page's per-page marker must round-trip
// through eviction with no corruption or race (128 pages, 64 frames → plenty of eviction churn).
TEST(BpmConcurrentTest, ConcurrentIntegrityUnderEviction) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(64, &dm);
  constexpr int kPages = 128;
  std::vector<page_id_t> pages;
  for (int i = 0; i < kPages; i++) {
    auto pid = bpm.NewPage();
    auto g = bpm.WritePage(pid);
    std::memset(g.GetDataMut(), static_cast<char>(i), PAGE_SIZE);
    pages.push_back(pid);
  }

  LaunchParallelTest(8, [&](uint64_t /*tid*/) {
    for (int rep = 0; rep < 50; rep++) {
      for (int i = 0; i < kPages; i++) {
        auto g = bpm.ReadPage(pages[i]);
        // Every byte of page i must be the marker i, even after surviving eviction to disk.
        ASSERT_EQ(g.GetData()[0], static_cast<char>(i));
        ASSERT_EQ(g.GetData()[PAGE_SIZE - 1], static_cast<char>(i));
      }
    }
  });
}

}  // namespace bumblebee
