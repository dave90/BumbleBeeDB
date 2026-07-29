//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// catalog_test.cpp
//
// Identification: test/unit/catalog/catalog_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "catalog/catalog.h"

#include "frontend_test_util.h"
#include "gtest/gtest.h"

namespace bumblebee {

TEST(CatalogTest, CreateAndLookUpByName) {
  Catalog catalog;
  auto info = catalog.CreateTable(
      "t", Schema(std::vector{Column{"v1", LogicalType(LogicalTypeId::INTEGER)},
                              Column{"v2", LogicalType(LogicalTypeId::STRING), 32}}));
  ASSERT_NE(info, NULL_TABLE_INFO);
  EXPECT_EQ(info->name_, "t");

  auto found = catalog.GetTable("t");
  ASSERT_NE(found, NULL_TABLE_INFO);
  EXPECT_EQ(found->oid_, info->oid_);
  EXPECT_EQ(found->schema_.GetColumnCount(), 2U);
  EXPECT_EQ(found->schema_.GetColumn(0).GetType(), LogicalType(LogicalTypeId::INTEGER));
  EXPECT_EQ(found->schema_.GetColIdx("v2"), 1U);
}

TEST(CatalogTest, LookUpByOid) {
  Catalog catalog;
  auto info = catalog.CreateTable("t", Schema(std::vector{Column{"v1", LogicalType(LogicalTypeId::INTEGER)}}));
  ASSERT_NE(info, NULL_TABLE_INFO);
  auto found = catalog.GetTable(info->oid_);
  ASSERT_NE(found, NULL_TABLE_INFO);
  EXPECT_EQ(found->name_, "t");
}

TEST(CatalogTest, MissingTableIsNull) {
  Catalog catalog;
  EXPECT_EQ(catalog.GetTable("nope"), NULL_TABLE_INFO);
  EXPECT_EQ(catalog.GetTable(static_cast<table_oid_t>(99)), NULL_TABLE_INFO);
}

TEST(CatalogTest, DuplicateNameIsRejected) {
  Catalog catalog;
  auto schema = Schema(std::vector{Column{"v1", LogicalType(LogicalTypeId::INTEGER)}});
  EXPECT_NE(catalog.CreateTable("t", schema), NULL_TABLE_INFO);
  EXPECT_EQ(catalog.CreateTable("t", schema), NULL_TABLE_INFO);
}

TEST(CatalogTest, OidsAreDistinct) {
  Catalog catalog;
  auto schema = Schema(std::vector{Column{"v1", LogicalType(LogicalTypeId::INTEGER)}});
  auto a = catalog.CreateTable("a", schema);
  auto b = catalog.CreateTable("b", schema);
  EXPECT_NE(a->oid_, b->oid_);
}

TEST(CatalogTest, GetTableNames) {
  auto catalog = MakeTestCatalog();
  auto names = catalog->GetTableNames();
  EXPECT_EQ(names.size(), 8U);
  EXPECT_NE(std::find(names.begin(), names.end(), "y"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "arr"), names.end());
}

// ---------------------------------------------------------------------------
// Schema / Column
// ---------------------------------------------------------------------------

TEST(SchemaTest, OffsetsAndInlining) {
  Schema schema(std::vector{Column{"a", LogicalType(LogicalTypeId::INTEGER)},
                            Column{"b", LogicalType(LogicalTypeId::BIGINT)},
                            Column{"c", LogicalType(LogicalTypeId::STRING), 64}});
  EXPECT_EQ(schema.GetColumn(0).GetOffset(), 0U);
  EXPECT_EQ(schema.GetColumn(1).GetOffset(), 4U);   // after a 4-byte INTEGER
  EXPECT_EQ(schema.GetColumn(2).GetOffset(), 12U);  // after an 8-byte BIGINT

  EXPECT_TRUE(schema.GetColumn(0).IsInlined());
  EXPECT_FALSE(schema.GetColumn(2).IsInlined());
  EXPECT_FALSE(schema.IsInlined());
  EXPECT_EQ(schema.GetUnlinedColumnCount(), 1U);
}

TEST(SchemaTest, BooleanIsOneByte) {
  Schema schema(std::vector{Column{"flag", LogicalType(LogicalTypeId::BOOLEAN)}});
  // BOOLEAN is physically a UTINYINT.
  EXPECT_EQ(schema.GetColumn(0).GetStorageSize(), 1U);
  EXPECT_TRUE(schema.IsInlined());
}

TEST(SchemaTest, ArrayColumnIsNotInlined) {
  Schema schema(std::vector{
      Column{"id", LogicalType(LogicalTypeId::INTEGER)},
      Column{"tags", LogicalType::List(LogicalTypeId::INTEGER), 0},
      Column{"fixed", LogicalType::Array(LogicalTypeId::INTEGER, 3), 0}});
  EXPECT_TRUE(schema.GetColumn(0).IsInlined());
  EXPECT_FALSE(schema.GetColumn(1).IsInlined());
  EXPECT_FALSE(schema.GetColumn(2).IsInlined());
  EXPECT_EQ(schema.GetUnlinedColumnCount(), 2U);
}

TEST(SchemaTest, CopySchema) {
  Schema schema(std::vector{Column{"a", LogicalType(LogicalTypeId::INTEGER)},
                            Column{"b", LogicalType(LogicalTypeId::BIGINT)},
                            Column{"c", LogicalType(LogicalTypeId::INTEGER)}});
  auto projected = Schema::CopySchema(&schema, {2, 0});
  EXPECT_EQ(projected.GetColumnCount(), 2U);
  EXPECT_EQ(projected.GetColumn(0).GetName(), "c");
  EXPECT_EQ(projected.GetColumn(1).GetName(), "a");
}

TEST(SchemaTest, ToString) {
  Schema schema(std::vector{Column{"a", LogicalType(LogicalTypeId::INTEGER)},
                            Column{"tags", LogicalType::List(LogicalTypeId::INTEGER), 0}});
  EXPECT_EQ(schema.ToString(), "(a:INTEGER, tags:INTEGER[])");
}

TEST(SchemaTest, MissingColumnThrows) {
  Schema schema(std::vector{Column{"a", LogicalType(LogicalTypeId::INTEGER)}});
  EXPECT_EQ(schema.TryGetColIdx("nope"), std::nullopt);
  EXPECT_THROW(schema.GetColIdx("nope"), Exception);
}

}  // namespace bumblebee
