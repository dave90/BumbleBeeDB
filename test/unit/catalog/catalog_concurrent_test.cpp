//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// catalog_concurrent_test.cpp
//
// Identification: test/unit/catalog/catalog_concurrent_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "concurrency_test_util.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"

namespace bumblebee {

// Phase 0.3 — concurrent DDL: many threads each create distinct tables; the catalog must register
// them all with no race (TSan-clean) and no lost registration.
TEST(CatalogConcurrentTest, ConcurrentCreateTable) {
  MemoryDiskManager dm(4096);
  BufferPoolManager bpm(64, &dm);
  Catalog catalog(&bpm);
  Schema schema{std::vector<Column>{Column("id", LogicalType(LogicalTypeId::INTEGER))}};

  const int threads = 8;
  const int per_thread = 10;
  LaunchParallelTest(threads, [&](uint64_t tid) {
    for (int i = 0; i < per_thread; i++) {
      catalog.CreateTable("t_" + std::to_string(tid) + "_" + std::to_string(i), schema);
    }
  });

  EXPECT_EQ(catalog.GetTableNames().size(), static_cast<size_t>(threads * per_thread));
  for (int t = 0; t < threads; t++) {
    for (int i = 0; i < per_thread; i++) {
      EXPECT_NE(catalog.GetTable("t_" + std::to_string(t) + "_" + std::to_string(i)), NULL_TABLE_INFO);
    }
  }
}

// Concurrent CreateTable + CreateIndex (each index on its own freshly created table).
TEST(CatalogConcurrentTest, ConcurrentCreateTableAndIndex) {
  MemoryDiskManager dm(8192);
  BufferPoolManager bpm(128, &dm);
  Catalog catalog(&bpm);
  Schema schema{std::vector<Column>{Column("id", LogicalType(LogicalTypeId::INTEGER))}};

  const int threads = 6;
  LaunchParallelTest(threads, [&](uint64_t tid) {
    auto name = "tbl_" + std::to_string(tid);
    catalog.CreateTable(name, schema);
    catalog.CreateIndex<8>("idx_" + std::to_string(tid), name, {0});
  });

  for (int t = 0; t < threads; t++) {
    auto name = "tbl_" + std::to_string(t);
    EXPECT_NE(catalog.GetTable(name), NULL_TABLE_INFO);
    EXPECT_NE(catalog.GetIndex("idx_" + std::to_string(t), name), NULL_INDEX_INFO);
  }
}

}  // namespace bumblebee
