//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parallel_scan_test.cpp
//
// Identification: test/unit/storage/table/parallel_scan_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/config.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

static auto MakeSchema() -> SchemaRef {
  std::vector<Column> cols{Column("id", LogicalType(LogicalTypeId::INTEGER))};
  return std::make_shared<Schema>(cols);
}

/** @brief Append `n` rows (id = 0..n-1) to the heap in chunks, spanning many pages. */
static void AppendRows(TableHeap &heap, int n) {
  const std::vector<LogicalType> types{LogicalType(LogicalTypeId::INTEGER)};
  int written = 0;
  while (written < n) {
    idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(types);
    for (idx_t i = 0; i < batch; i++) {
      chunk.SetValue(0, i, Value(written + static_cast<int>(i)));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap.Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

/** @brief Drain a HeapScan cursor, collecting every id it yields into `out`. */
static void DrainInto(TableScan &scan, std::vector<int> &out) {
  const std::vector<LogicalType> types{LogicalType(LogicalTypeId::INTEGER)};
  while (true) {
    DataChunk chunk;
    chunk.Initialize(types);
    if (!scan.Next(chunk, nullptr)) {
      break;
    }
    for (idx_t i = 0; i < chunk.GetSize(); i++) {
      out.push_back(chunk.GetValue(0, i).GetAs<int>());
    }
  }
}

TEST(ParallelScanTest, MorselsCoverEveryRowExactlyOnce) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(128, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  constexpr int kRows = 50000;  // spans many pages -> many morsels
  AppendRows(heap, kRows);

  auto state = heap.BeginParallelScan(nullptr, nullptr, 0);
  ASSERT_GT(state->NumPages(), MORSEL_PAGES);  // enough pages that morsels actually split the work

  std::mutex mu;
  std::vector<int> all;
  std::atomic<int> morsels{0};

  auto worker = [&] {
    std::vector<int> local;
    idx_t begin;
    idx_t end;
    while (state->NextMorsel(begin, end)) {
      morsels.fetch_add(1, std::memory_order_relaxed);
      auto scan = heap.MakeMorselScan(state, begin, end);
      DrainInto(*scan, local);
    }
    std::lock_guard lock(mu);
    all.insert(all.end(), local.begin(), local.end());
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back(worker);
  }
  for (auto &t : threads) {
    t.join();
  }

  ASSERT_EQ(all.size(), kRows);
  std::unordered_set<int> seen(all.begin(), all.end());
  EXPECT_EQ(seen.size(), kRows);  // every id distinct -> each row seen exactly once
  for (int i = 0; i < kRows; i++) {
    EXPECT_TRUE(seen.contains(i)) << "missing row " << i;
  }
  EXPECT_GT(morsels.load(), 1);  // the scan really was split across morsels
}

TEST(ParallelScanTest, SingleCursorMakeScanStillCoversEveryRow) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(128, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  constexpr int kRows = 12345;
  AppendRows(heap, kRows);

  auto scan = heap.MakeScan();
  std::vector<int> all;
  DrainInto(*scan, all);

  ASSERT_EQ(all.size(), kRows);
  std::unordered_set<int> seen(all.begin(), all.end());
  EXPECT_EQ(seen.size(), kRows);
}

TEST(ParallelScanTest, HalloweenBoundaryExcludesLaterAppends) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(128, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  constexpr int kInitial = 5000;
  AppendRows(heap, kInitial);

  // Open the scan, THEN append more rows. The snapshot must not see the late arrivals.
  auto state = heap.BeginParallelScan(nullptr, nullptr, 0);
  AppendRows(heap, 3000);  // ids 5000..7999, appended after the snapshot

  std::vector<int> all;
  idx_t begin;
  idx_t end;
  while (state->NextMorsel(begin, end)) {
    auto scan = heap.MakeMorselScan(state, begin, end);
    DrainInto(*scan, all);
  }

  EXPECT_EQ(all.size(), kInitial);
  for (int id : all) {
    EXPECT_LT(id, kInitial) << "saw a row appended after the scan opened";
  }
}

}  // namespace bumblebee
