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

// Concurrent DropTable: pre-create a grid of tables (each with a PK index), then many threads each drop
// their own disjoint set. DropTable mutates four maps (tables/table_names/indexes/index_names) under the
// catalog latch, so the run must be race-free and drop every table AND its index with none left behind.
TEST(CatalogConcurrentTest, ConcurrentDropDistinctTables) {
  MemoryDiskManager dm(16384);
  BufferPoolManager bpm(256, &dm);
  Catalog catalog(&bpm);
  Schema schema{std::vector<Column>{Column("id", LogicalType(LogicalTypeId::INTEGER))}};

  const int threads = 8;
  const int per_thread = 8;
  for (int t = 0; t < threads; t++) {
    for (int i = 0; i < per_thread; i++) {
      auto name = "d_" + std::to_string(t) + "_" + std::to_string(i);
      catalog.CreateTable(name, schema);
      catalog.CreateIndex<8>("_pk_" + name, name, {0});
    }
  }
  ASSERT_EQ(catalog.GetTables().size(), static_cast<size_t>(threads * per_thread));
  ASSERT_EQ(catalog.GetIndexes().size(), static_cast<size_t>(threads * per_thread));

  LaunchParallelTest(threads, [&](uint64_t tid) {
    for (int i = 0; i < per_thread; i++) {
      EXPECT_TRUE(catalog.DropTable("d_" + std::to_string(tid) + "_" + std::to_string(i)));
    }
  });

  // Every table and its index is gone.
  EXPECT_TRUE(catalog.GetTables().empty());
  EXPECT_TRUE(catalog.GetIndexes().empty());
  EXPECT_TRUE(catalog.GetTableNames().empty());
}

// Concurrent CREATE + DROP + lookups on the same catalog. Each thread churns its own namespace
// (create then drop) while every thread's reads (GetTableNames / GetTable) race the others' writes.
// The latch must keep those reads internally consistent (no crash / torn map), and because each thread
// drops everything it created, the catalog must end up empty.
TEST(CatalogConcurrentTest, ConcurrentCreateDropAndLookup) {
  MemoryDiskManager dm(16384);
  BufferPoolManager bpm(256, &dm);
  Catalog catalog(&bpm);
  Schema schema{std::vector<Column>{Column("id", LogicalType(LogicalTypeId::INTEGER))}};

  const int threads = 8;
  const int rounds = 30;
  LaunchParallelTest(threads, [&](uint64_t tid) {
    for (int r = 0; r < rounds; r++) {
      auto name = "m_" + std::to_string(tid) + "_" + std::to_string(r);
      catalog.CreateTable(name, schema);
      // Reads that race other threads' concurrent create/drop — must not crash or observe a torn map.
      auto names = catalog.GetTableNames();
      EXPECT_LE(names.size(), static_cast<size_t>(threads * rounds));
      // Our own just-created table is visible to us (same-thread happens-before).
      EXPECT_NE(catalog.GetTable(name), NULL_TABLE_INFO);
      EXPECT_TRUE(catalog.DropTable(name));
    }
  });

  EXPECT_TRUE(catalog.GetTables().empty());
}

}  // namespace bumblebee
