//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// catalog_storage_test.cpp
//
// Identification: test/unit/storage/catalog_storage_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <memory>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

static auto MakeSchema() -> Schema {
  return Schema{std::vector<Column>{
      Column("id", LogicalType(LogicalTypeId::INTEGER)),
      Column("v", LogicalType(LogicalTypeId::DOUBLE)),
  }};
}

TEST(CatalogStorageTest, CreateTableWithBpmGetsRowStorage) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto info = catalog.CreateTable("t", MakeSchema());
  ASSERT_NE(info, NULL_TABLE_INFO);
  ASSERT_NE(info->storage_, nullptr);
  EXPECT_EQ(info->storage_->GetFormat(), StorageFormat::ROW);

  // Append and scan through the catalog-owned storage.
  DataChunk in;
  in.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE)});
  in.SetValue(0, 0, Value(42));
  in.SetValue(1, 0, Value(3.5));
  in.SetCardinality(1);
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  info->storage_->Append(in, rids);

  auto scan = info->storage_->MakeScan();
  DataChunk out;
  out.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE)});
  ASSERT_TRUE(scan->Next(out));
  EXPECT_EQ(out.GetValue(0, 0), Value(42));
  EXPECT_EQ(out.GetValue(1, 0), Value(3.5));
}

TEST(CatalogStorageTest, MetadataOnlyCatalogHasNoStorage) {
  Catalog catalog;  // no buffer pool
  auto info = catalog.CreateTable("t", MakeSchema());
  ASSERT_NE(info, NULL_TABLE_INFO);
  EXPECT_EQ(info->storage_, nullptr);  // frontend path: table has metadata but no rows
  EXPECT_EQ(catalog.GetTable("t"), info);
}

TEST(CatalogStorageTest, ParquetFormatCreatesExternalStorage) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);
  auto info = catalog.CreateTable("p", MakeSchema(), StorageFormat::PARQUET, {}, false, "/tmp/p_loc");
  ASSERT_NE(info, NULL_TABLE_INFO);
  ASSERT_NE(info->storage_, nullptr);
  EXPECT_EQ(info->storage_->GetFormat(), StorageFormat::PARQUET);
  EXPECT_EQ(static_cast<ParquetTable *>(info->storage_.get())->GetPath(), "/tmp/p_loc");
}

TEST(CatalogStorageTest, DropTableRemovesTableAndIndex) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto info = catalog.CreateTable("t", MakeSchema(), StorageFormat::ROW, {0}, /*auto_id=*/false);
  ASSERT_NE(info, NULL_TABLE_INFO);
  ASSERT_NE(catalog.CreateIndexForKey("_pk_t", "t", {0}), NULL_INDEX_INFO);
  EXPECT_EQ(catalog.GetTableIndexes("t").size(), 1U);

  // Dropping the table succeeds and takes the table and its index with it.
  EXPECT_TRUE(catalog.DropTable("t"));
  EXPECT_EQ(catalog.GetTable("t"), NULL_TABLE_INFO);
  EXPECT_EQ(catalog.GetIndex("_pk_t", "t"), NULL_INDEX_INFO);
  EXPECT_TRUE(catalog.GetTableIndexes("t").empty());
  EXPECT_TRUE(catalog.GetTables().empty());
  EXPECT_TRUE(catalog.GetIndexes().empty());
}

TEST(CatalogStorageTest, DropMissingTableReturnsFalse) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);
  EXPECT_FALSE(catalog.DropTable("nope"));
}

TEST(CatalogStorageTest, DropTableDoesNotReuseOidAndSparesOthers) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto a = catalog.CreateTable("a", MakeSchema());
  auto b = catalog.CreateTable("b", MakeSchema());
  ASSERT_NE(a, NULL_TABLE_INFO);
  ASSERT_NE(b, NULL_TABLE_INFO);

  EXPECT_TRUE(catalog.DropTable("a"));
  // The sibling table is untouched.
  EXPECT_EQ(catalog.GetTable("b"), b);

  // Recreating a dropped name is allowed, but gets a FRESH oid (the allocator is never rewound).
  auto a2 = catalog.CreateTable("a", MakeSchema());
  ASSERT_NE(a2, NULL_TABLE_INFO);
  EXPECT_NE(a2->oid_, a->oid_);
  EXPECT_NE(a2->oid_, b->oid_);
}

static auto Contains(const std::vector<page_id_t> &pages, page_id_t p) -> bool {
  return std::find(pages.begin(), pages.end(), p) != pages.end();
}

static auto HeapOf(const std::shared_ptr<TableInfo> &info) -> TableHeap * {
  return dynamic_cast<TableHeap *>(info->storage_.get());
}

TEST(CatalogStorageTest, DropTableReturnsHeapPagesToFreeList) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto info = catalog.CreateTable("t", MakeSchema());
  ASSERT_NE(info, NULL_TABLE_INFO);
  auto *heap = HeapOf(info);
  ASSERT_NE(heap, nullptr);

  // Append enough rows that the heap grows past its initial page, so we can prove the WHOLE chain
  // is reclaimed, not just the first page. A chunk holds at most STANDARD_VECTOR_SIZE rows, so we
  // append several batches; ~3000 rows of (INT, DOUBLE) span multiple 8 KiB pages.
  const idx_t kBatch = STANDARD_VECTOR_SIZE;
  const idx_t kBatches = 3;
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  for (idx_t b = 0; b < kBatches; b++) {
    DataChunk in;
    in.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE)});
    for (idx_t i = 0; i < kBatch; i++) {
      in.SetValue(0, i, Value(static_cast<int32_t>(i)));
      in.SetValue(1, i, Value(static_cast<double>(i)));
    }
    in.SetCardinality(kBatch);
    heap->Append(in, rids);
  }

  const page_id_t first = heap->GetFirstPageId();
  EXPECT_TRUE(bpm.GetFreePages().empty());  // nothing reclaimed yet

  ASSERT_TRUE(catalog.DropTable("t"));

  auto freed = bpm.GetFreePages();
  EXPECT_GE(freed.size(), 2U) << "a multi-page heap should free every page it owned";
  EXPECT_TRUE(Contains(freed, first)) << "the heap's first page must be reclaimed";
}

TEST(CatalogStorageTest, DropTableReturnsIndexPagesToFreeList) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto info = catalog.CreateTable("t", MakeSchema(), StorageFormat::ROW, {0}, /*auto_id=*/false);
  ASSERT_NE(info, NULL_TABLE_INFO);
  auto index = catalog.CreateIndexForKey("_pk_t", "t", {0});
  ASSERT_NE(index, NULL_INDEX_INFO);

  const page_id_t index_header = index->index_->GetHeaderPageId();
  const page_id_t heap_first = HeapOf(info)->GetFirstPageId();
  EXPECT_NE(index_header, INVALID_PAGE_ID);

  ASSERT_TRUE(catalog.DropTable("t"));

  auto freed = bpm.GetFreePages();
  EXPECT_TRUE(Contains(freed, index_header)) << "the index's B+ tree header page must be reclaimed";
  EXPECT_TRUE(Contains(freed, heap_first)) << "the table's heap page must also be reclaimed";
}

TEST(CatalogStorageTest, PagesFreedByDropAreReusedByNextCreate) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto a = catalog.CreateTable("a", MakeSchema());
  ASSERT_NE(a, NULL_TABLE_INFO);
  const page_id_t a_first = HeapOf(a)->GetFirstPageId();

  ASSERT_TRUE(catalog.DropTable("a"));
  auto freed = bpm.GetFreePages();
  ASSERT_FALSE(freed.empty());

  // The next CREATE draws from the free list instead of extending the file — the just-freed page
  // comes straight back (NewPage reuses reclaimed ids before allocating fresh ones).
  auto b = catalog.CreateTable("b", MakeSchema());
  ASSERT_NE(b, NULL_TABLE_INFO);
  const page_id_t b_first = HeapOf(b)->GetFirstPageId();
  EXPECT_TRUE(Contains(freed, b_first)) << "CREATE after DROP should reuse a freed page id";
  EXPECT_EQ(b_first, a_first);
}

TEST(CatalogStorageTest, DropAllTablesReturnsEveryPageToFreeList) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);

  auto a = catalog.CreateTable("a", MakeSchema(), StorageFormat::ROW, {0}, /*auto_id=*/false);
  ASSERT_NE(a, NULL_TABLE_INFO);
  ASSERT_NE(catalog.CreateIndexForKey("_pk_a", "a", {0}), NULL_INDEX_INFO);
  auto b = catalog.CreateTable("b", MakeSchema());
  ASSERT_NE(b, NULL_TABLE_INFO);

  const page_id_t a_first = HeapOf(a)->GetFirstPageId();
  const page_id_t b_first = HeapOf(b)->GetFirstPageId();

  EXPECT_EQ(catalog.DropAllTables(), 2U);

  auto freed = bpm.GetFreePages();
  EXPECT_TRUE(Contains(freed, a_first));
  EXPECT_TRUE(Contains(freed, b_first));
  EXPECT_GE(freed.size(), 3U) << "a's heap page + a's index header + b's heap page";
}

}  // namespace bumblebee
