//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// external_table_write_test.cpp
//
// Identification: test/unit/storage/parquet/external_table_write_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <gtest/gtest.h>
#include <utime.h>

#include <filesystem>
#include <sstream>

#include "bumblebee_instance.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/parquet/parquet_table_ops.h"
#include "storage/table/parquet_table.h"

namespace bumblebee {

namespace fs = std::filesystem;

/** The non-transactional write path: manifest advancement, the fail-fast writer lock, and the
 * file-granular copy-on-write rewrite. */
class ExternalTableWriteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("bumblebee_ext_write_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    Run("CREATE TABLE t (a INT, b VARCHAR) WITH (format='parquet', location='" + Loc() + "');");
  }

  void TearDown() override { fs::remove_all(dir_); }

  auto Run(const std::string &sql) -> std::string {
    std::stringstream ss;
    SimpleStreamWriter writer(ss);
    instance_.ExecuteSql(sql, writer);
    return ss.str();
  }

  auto Loc() -> std::string { return dir_.string(); }

  auto Storage() -> ParquetTable * {
    return static_cast<ParquetTable *>(instance_.catalog_->GetTable("t")->storage_.get());
  }

  auto Manifest() -> ParquetManifest {
    auto m = ParquetManifestIO::ReadLatest(Loc());
    EXPECT_TRUE(m.has_value());
    return *m;
  }

  BumbleBeeInstance instance_;
  fs::path dir_;
};

TEST_F(ExternalTableWriteTest, InsertAdvancesManifestAndAppendsOnePartFile) {
  EXPECT_EQ(Manifest().version_, 0);
  Run("INSERT INTO t VALUES (1,'x'),(2,'y');");
  auto m1 = Manifest();
  EXPECT_EQ(m1.version_, 1);
  ASSERT_EQ(m1.entries_.size(), 1u);
  EXPECT_EQ(m1.entries_[0].row_count_, 2u);
  EXPECT_TRUE(fs::exists(dir_ / m1.entries_[0].file_name_));

  Run("INSERT INTO t VALUES (3,'z');");
  auto m2 = Manifest();
  EXPECT_EQ(m2.version_, 2);
  EXPECT_EQ(m2.entries_.size(), 2u);
  EXPECT_EQ(m2.TotalRows(), 3u);
}

TEST_F(ExternalTableWriteTest, DeleteAllYieldsEmptyManifestAndUnlinksParts) {
  Run("INSERT INTO t VALUES (1,'x'),(2,'y');");
  auto part = Manifest().entries_[0].file_name_;
  Run("DELETE FROM t;");
  auto m = Manifest();
  EXPECT_TRUE(m.entries_.empty());
  EXPECT_FALSE(fs::exists(dir_ / part)) << "the replaced part file must be unlinked";
  EXPECT_EQ(Run("SELECT COUNT(*) FROM t;"), "__unnamed#0\t\n0\t\n");
}

TEST_F(ExternalTableWriteTest, FileGranularRewriteCarriesUntouchedFilesForward) {
  Run("INSERT INTO t VALUES (1,'a'),(2,'b');");  // part file A (rows 1,2)
  Run("INSERT INTO t VALUES (3,'c'),(4,'d');");  // part file B (rows 3,4)
  auto before = Manifest();
  ASSERT_EQ(before.entries_.size(), 2u);
  const auto file_a = before.entries_[0].file_name_;
  const auto file_b = before.entries_[1].file_name_;

  // Deleting a row that lives only in file B must leave file A untouched (same name, same file).
  Run("DELETE FROM t WHERE a = 4;");
  auto after = Manifest();
  ASSERT_EQ(after.entries_.size(), 2u);
  EXPECT_EQ(after.entries_[0].file_name_, file_a);
  EXPECT_NE(after.entries_[1].file_name_, file_b);
  EXPECT_FALSE(fs::exists(dir_ / file_b)) << "the rewritten original must be unlinked";
  EXPECT_EQ(after.entries_[1].row_count_, 1u);
  EXPECT_EQ(Run("SELECT COUNT(*) FROM t;"), "__unnamed#0\t\n3\t\n");
}

TEST_F(ExternalTableWriteTest, ConcurrentWriterFailsImmediately) {
  Run("INSERT INTO t VALUES (1,'x');");
  // Simulate an in-flight writer holding the table lock: a second write must throw, not wait.
  ASSERT_TRUE(Storage()->TryLockForWrite());
  EXPECT_THROW(Run("INSERT INTO t VALUES (2,'y');"), ExecutionException);
  Storage()->UnlockWrite();
  // With the lock released the write goes through.
  Run("INSERT INTO t VALUES (2,'y');");
  EXPECT_EQ(Run("SELECT COUNT(*) FROM t;"), "__unnamed#0\t\n2\t\n");
}

TEST_F(ExternalTableWriteTest, CrossProcessLockFileBlocksAndGoesStale) {
  Run("INSERT INTO t VALUES (1,'x');");
  // A foreign (fresh) lock file blocks the write even though the in-process mutex is free.
  auto lock_path = dir_ / ParquetManifestIO::LOCK_FILE;
  { std::ofstream(lock_path) << "held"; }
  EXPECT_THROW(Run("INSERT INTO t VALUES (2,'y');"), ExecutionException);

  // Backdate it beyond the staleness threshold: it is treated as leaked by a crash and replaced.
  struct utimbuf old_times {};
  old_times.actime = old_times.modtime = time(nullptr) - ExternalWriteGuard::STALE_LOCK_SECONDS - 5;
  ASSERT_EQ(utime(lock_path.c_str(), &old_times), 0);
  Run("INSERT INTO t VALUES (2,'y');");
  EXPECT_EQ(Run("SELECT COUNT(*) FROM t;"), "__unnamed#0\t\n2\t\n");
  EXPECT_FALSE(fs::exists(lock_path)) << "the guard removes its lock file on release";
}

TEST_F(ExternalTableWriteTest, UpdateRewritesOnlyTouchedFiles) {
  Run("INSERT INTO t VALUES (1,'a'),(2,'b');");
  Run("INSERT INTO t VALUES (3,'c');");
  auto before = Manifest();
  const auto file_a = before.entries_[0].file_name_;

  Run("UPDATE t SET b = 'C' WHERE a = 3;");
  auto after = Manifest();
  ASSERT_EQ(after.entries_.size(), 2u);
  EXPECT_EQ(after.entries_[0].file_name_, file_a) << "file A holds no matched row and must carry forward";
  EXPECT_EQ(Run("SELECT b FROM t WHERE a = 3;"), "t.b\t\n'C'\t\n");
}

}  // namespace bumblebee
