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

#include <memory>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

namespace {

auto MakeSchema() -> Schema {
  return Schema{std::vector<Column>{
      Column("id", LogicalType(LogicalTypeId::INTEGER)),
      Column("v", LogicalType(LogicalTypeId::DOUBLE)),
  }};
}

}  // namespace

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

TEST(CatalogStorageTest, ParquetFormatNotImplemented) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  Catalog catalog(&bpm);
  EXPECT_THROW(catalog.CreateTable("p", MakeSchema(), StorageFormat::PARQUET), NotImplementedException);
}

}  // namespace bumblebee
