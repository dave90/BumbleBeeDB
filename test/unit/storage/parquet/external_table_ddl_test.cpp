//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// external_table_ddl_test.cpp
//
// Identification: test/unit/storage/parquet/external_table_ddl_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "bumblebee_instance.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/table/parquet_table.h"

namespace bumblebee {

namespace fs = std::filesystem;

/** DDL-level behavior of external parquet tables: WITH options, inference, validation, manifest
 * materialization, and the heap-machinery opt-outs (no _id / PK / index). */
class ExternalTableDdlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("bumblebee_ext_ddl_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    // A real parquet file to adopt: i INT32, j STRING (from the reader-test corpus).
    fs::path corpus = fs::path(__FILE__).parent_path() / "data" / "t1.parquet";
    fs::copy_file(corpus, dir_ / "t1.parquet");
  }

  void TearDown() override { fs::remove_all(dir_); }

  auto Run(const std::string &sql) -> std::string {
    std::stringstream ss;
    SimpleStreamWriter writer(ss);
    instance_.ExecuteSql(sql, writer);
    return ss.str();
  }

  auto Loc() -> std::string { return dir_.string(); }

  BumbleBeeInstance instance_;
  fs::path dir_;
};

TEST_F(ExternalTableDdlTest, InferenceAdoptsFilesAndMaterializesManifest) {
  Run("CREATE TABLE t () WITH (format='parquet', location='" + Loc() + "');");

  auto info = instance_.GetCatalog().GetTable("t");
  ASSERT_NE(info, NULL_TABLE_INFO);
  EXPECT_EQ(info->storage_->GetFormat(), StorageFormat::PARQUET);
  // Inferred schema: exactly the file's columns — no auto _id, no PK.
  ASSERT_EQ(info->schema_.GetColumnCount(), 2u);
  EXPECT_EQ(info->schema_.GetColumn(0).GetName(), "i");
  EXPECT_EQ(info->schema_.GetColumn(0).GetType().GetTypeId(), LogicalTypeId::INTEGER);
  EXPECT_EQ(info->schema_.GetColumn(1).GetName(), "j");
  EXPECT_EQ(info->schema_.GetColumn(1).GetType().GetTypeId(), LogicalTypeId::STRING);
  EXPECT_TRUE(info->pk_attrs_.empty());
  EXPECT_FALSE(info->auto_id_);
  EXPECT_TRUE(instance_.GetCatalog().GetTableIndexes("t").empty());

  // Adoption materialized manifest 0 listing the file with its footer row count.
  auto manifest = ParquetManifestIO::ReadLatest(Loc());
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->version_, 0);
  ASSERT_EQ(manifest->entries_.size(), 1u);
  EXPECT_EQ(manifest->entries_[0].file_name_, "t1.parquet");
  EXPECT_EQ(manifest->entries_[0].row_count_, 1u);
}

TEST_F(ExternalTableDdlTest, DeclaredColumnsMustMatchFiles) {
  // Wrong type.
  EXPECT_THROW(Run("CREATE TABLE t (i BIGINT, j VARCHAR) WITH (format='parquet', location='" + Loc() + "');"),
               Exception);
  // Wrong count.
  EXPECT_THROW(Run("CREATE TABLE t (i INT) WITH (format='parquet', location='" + Loc() + "');"), Exception);
  // Wrong name.
  EXPECT_THROW(Run("CREATE TABLE t (i INT, k VARCHAR) WITH (format='parquet', location='" + Loc() + "');"), Exception);
  // Matching declaration (names case-insensitive, INT32 -> INTEGER) succeeds.
  Run("CREATE TABLE t (I INT, J VARCHAR) WITH (format='parquet', location='" + Loc() + "');");
  EXPECT_NE(instance_.GetCatalog().GetTable("t"), NULL_TABLE_INFO);
}

TEST_F(ExternalTableDdlTest, EmptyFolderNeedsDeclaredColumns) {
  auto empty = (dir_ / "empty_sub").string();
  EXPECT_THROW(Run("CREATE TABLE t () WITH (format='parquet', location='" + empty + "');"), Exception);

  // With declared columns an empty folder becomes an empty external table with manifest 0.
  Run("CREATE TABLE t (a INT, b VARCHAR) WITH (format='parquet', location='" + empty + "');");
  auto manifest = ParquetManifestIO::ReadLatest(empty);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->version_, 0);
  EXPECT_TRUE(manifest->entries_.empty());
}

TEST_F(ExternalTableDdlTest, OptionValidation) {
  EXPECT_THROW(Run("CREATE TABLE t (a INT) WITH (format='csv', location='/tmp/x');"), BinderException);
  EXPECT_THROW(Run("CREATE TABLE t (a INT) WITH (format='parquet');"), BinderException);
  EXPECT_THROW(Run("CREATE TABLE t (a INT) WITH (location='/tmp/x');"), BinderException);
  EXPECT_THROW(Run("CREATE TABLE t (a INT) WITH (whatever='x');"), BinderException);
  EXPECT_THROW(Run("CREATE TABLE t (a INT PRIMARY KEY) WITH (format='parquet', location='/tmp/x');"), BinderException);
  // Empty column list without external format stays an error (and must not crash).
  EXPECT_THROW(Run("CREATE TABLE t ();"), BinderException);
}

TEST_F(ExternalTableDdlTest, ExistingManifestIsAuthoritative) {
  // A manifest that lists no files makes directory contents invisible: schema inference must fail
  // even though t1.parquet sits in the folder.
  ParquetManifest manifest;
  manifest.version_ = 7;
  ParquetManifestIO::Write(Loc(), manifest);

  EXPECT_THROW(Run("CREATE TABLE t () WITH (format='parquet', location='" + Loc() + "');"), Exception);

  // And CREATE must not overwrite the existing manifest with an adoption listing.
  auto latest = ParquetManifestIO::ReadLatest(Loc());
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->version_, 7);
  EXPECT_TRUE(latest->entries_.empty());
}

TEST_F(ExternalTableDdlTest, ExternalTableSurvivesReopen) {
  auto db_file = dir_ / "meta.db";
  {
    BumbleBeeInstance durable(db_file, 64, std::chrono::hours(2));
    std::stringstream ss;
    SimpleStreamWriter writer(ss);
    durable.ExecuteSql("CREATE TABLE ext () WITH (format='parquet', location='" + Loc() + "');", writer);
    durable.ExecuteSql("CREATE TABLE plain (a INT);", writer);
  }
  // Reopen: the external table comes back with its schema, format and location intact.
  BumbleBeeInstance durable(db_file, 64, std::chrono::hours(2));
  auto info = durable.GetCatalog().GetTable("ext");
  ASSERT_NE(info, NULL_TABLE_INFO);
  ASSERT_NE(info->storage_, nullptr);
  EXPECT_EQ(info->storage_->GetFormat(), StorageFormat::PARQUET);
  EXPECT_EQ(static_cast<ParquetTable *>(info->storage_.get())->GetPath(), Loc());
  ASSERT_EQ(info->schema_.GetColumnCount(), 2u);
  EXPECT_EQ(info->schema_.GetColumn(0).GetName(), "i");
  // The row-format table reloaded alongside it.
  EXPECT_NE(durable.GetCatalog().GetTable("plain"), NULL_TABLE_INFO);
}

TEST_F(ExternalTableDdlTest, DropKeepsDataFiles) {
  Run("CREATE TABLE t () WITH (format='parquet', location='" + Loc() + "');");
  Run("DROP TABLE t;");
  EXPECT_EQ(instance_.GetCatalog().GetTable("t"), NULL_TABLE_INFO);
  // External semantics: dropping the table never deletes the data.
  EXPECT_TRUE(fs::exists(dir_ / "t1.parquet"));
}

}  // namespace bumblebee
