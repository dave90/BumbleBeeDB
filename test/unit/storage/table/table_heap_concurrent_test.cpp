//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_heap_concurrent_test.cpp
//
// Identification: test/unit/storage/table/table_heap_concurrent_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <memory>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "concurrency_test_util.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

static auto MakeSchema() -> SchemaRef {
  return std::make_shared<Schema>(std::vector<Column>{
      Column("id", LogicalType(LogicalTypeId::INTEGER)),
      Column("v", LogicalType(LogicalTypeId::DOUBLE)),
  });
}

static void AppendOne(TableHeap &heap, const Schema &schema, int32_t id, double v) {
  DataChunk chunk;
  chunk.Initialize(schema.GetTypes());
  chunk.SetValue(0, 0, Value(id));
  chunk.SetValue(1, 0, Value(v));
  chunk.SetCardinality(1);
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(chunk, rids);
}

static auto CountScan(TableHeap &heap, const Schema &schema) -> int {
  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(schema.GetTypes());
  int seen = 0;
  while (scan->Next(out)) {
    seen += static_cast<int>(out.GetSize());
  }
  return seen;
}

// Phase 0.2 — Halloween fix: a scan opened before further appends must NOT see the later rows.
TEST(TableHeapConcurrentTest, ScanStopBoundExcludesLaterAppends) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(32, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  for (int i = 0; i < 5; i++) {
    AppendOne(heap, *schema, i, i);
  }
  // Open the scan (snapshots the end bound) BEFORE appending more rows.
  auto scan = heap.MakeScan();
  for (int i = 5; i < 50; i++) {
    AppendOne(heap, *schema, i, i);
  }
  DataChunk out;
  out.Initialize(schema->GetTypes());
  int seen = 0;
  while (scan->Next(out)) {
    seen += static_cast<int>(out.GetSize());
  }
  EXPECT_EQ(seen, 5) << "scan must not observe rows appended after it opened";

  // A fresh scan now sees everything.
  EXPECT_EQ(CountScan(heap, *schema), 50);
}

// Concurrent appenders + a concurrent scanner: no crash / race (TSan), and every appended row is
// eventually visible to a scan opened after the writers finish.
TEST(TableHeapConcurrentTest, ConcurrentAppendThenScan) {
  MemoryDiskManager dm(4096);
  BufferPoolManager bpm(64, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  const int per_thread = 200;
  const int threads = 4;
  // Run a scanner concurrently to stress read/write interleaving (its result count is nondeterministic).
  std::atomic<bool> stop{false};
  std::thread scanner([&]() {
    while (!stop.load()) {
      volatile int c = CountScan(heap, *schema);
      (void)c;
    }
  });

  LaunchParallelTest(threads, [&](uint64_t tid) {
    auto base = static_cast<int>(tid) * per_thread;
    for (int i = 0; i < per_thread; i++) {
      AppendOne(heap, *schema, base + i, base + i);
    }
  });
  stop.store(true);
  scanner.join();

  EXPECT_EQ(CountScan(heap, *schema), threads * per_thread);
}

}  // namespace bumblebee
