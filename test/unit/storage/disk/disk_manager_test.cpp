//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// disk_manager_test.cpp
//
// Identification: test/unit/storage/disk/disk_manager_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <filesystem>
#include <string>

#include "common/config.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/disk/single_file_disk_manager.h"

namespace bumblebee {

namespace {

auto TempDbPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / name;
}

void FillPage(data_ptr_t buf, data_t value) { std::memset(buf, value, PAGE_SIZE); }

}  // namespace

TEST(SingleFileDiskManagerTest, WriteThenReadRoundTrips) {
  auto path = TempDbPath("bbdb_dm_roundtrip.db");
  std::filesystem::remove(path);
  {
    SingleFileDiskManager dm(path);
    data_t write_buf[PAGE_SIZE];
    data_t read_buf[PAGE_SIZE];
    FillPage(write_buf, 'A');
    ASSERT_TRUE(dm.WritePage(0, write_buf));
    ASSERT_TRUE(dm.ReadPage(0, read_buf));
    EXPECT_EQ(0, std::memcmp(write_buf, read_buf, PAGE_SIZE));
    EXPECT_EQ(dm.GetNumWrites(), 1);
  }
  std::filesystem::remove(path);
}

// Bug #4a: reading a page that was never written must zero-fill and must NOT allocate disk space.
TEST(SingleFileDiskManagerTest, ReadUnknownPageDoesNotAllocate) {
  auto path = TempDbPath("bbdb_dm_read_noalloc.db");
  std::filesystem::remove(path);
  {
    SingleFileDiskManager dm(path);
    auto size_before = dm.GetDbFileSize();

    data_t read_buf[PAGE_SIZE];
    FillPage(read_buf, 'Z');  // poison
    ASSERT_TRUE(dm.ReadPage(42, read_buf));

    data_t zero[PAGE_SIZE];
    FillPage(zero, 0);
    EXPECT_EQ(0, std::memcmp(read_buf, zero, PAGE_SIZE)) << "unwritten page must read as zeros";

    // No write occurred, and the file did not grow as a side effect of the read.
    EXPECT_EQ(dm.GetNumWrites(), 0);
    EXPECT_EQ(dm.GetDbFileSize(), size_before);
  }
  std::filesystem::remove(path);
}

// Positional layout: page N lives at a fixed offset, so distinct ids are independent, and deleting a
// page zero-fills its slot (it reads back empty). Page-id reuse is the buffer pool's job, not here.
TEST(SingleFileDiskManagerTest, PositionalOffsetsAndDeleteZeroFills) {
  auto path = TempDbPath("bbdb_dm_positional.db");
  std::filesystem::remove(path);
  {
    SingleFileDiskManager dm(path);
    data_t buf[PAGE_SIZE];
    data_t read_buf[PAGE_SIZE];
    FillPage(buf, 'A');
    ASSERT_TRUE(dm.WritePage(1, buf));
    FillPage(buf, 'B');
    ASSERT_TRUE(dm.WritePage(5, buf));  // a different fixed slot

    ASSERT_TRUE(dm.ReadPage(1, read_buf));
    EXPECT_EQ(read_buf[0], 'A');
    ASSERT_TRUE(dm.ReadPage(5, read_buf));
    EXPECT_EQ(read_buf[0], 'B');

    dm.DeletePage(1);
    data_t zero[PAGE_SIZE];
    FillPage(zero, 0);
    ASSERT_TRUE(dm.ReadPage(1, read_buf));
    EXPECT_EQ(0, std::memcmp(read_buf, zero, PAGE_SIZE)) << "a deleted page reads back as zeros";
    ASSERT_TRUE(dm.ReadPage(5, read_buf));
    EXPECT_EQ(read_buf[0], 'B') << "an unrelated page is untouched by the delete";
  }
  std::filesystem::remove(path);
}

// Durability: pages written by one manager (including one that grows the file past the default) are
// read back intact by a freshly constructed manager on the same file — the restart proof for the
// self-describing positional layout.
TEST(SingleFileDiskManagerTest, ReopenReadsBackWrittenPages) {
  auto path = TempDbPath("bbdb_dm_reopen.db");
  std::filesystem::remove(path);
  {
    SingleFileDiskManager dm(path);
    data_t buf[PAGE_SIZE];
    FillPage(buf, 'a');
    ASSERT_TRUE(dm.WritePage(0, buf));
    FillPage(buf, 'd');
    ASSERT_TRUE(dm.WritePage(3, buf));
    FillPage(buf, 'z');
    ASSERT_TRUE(dm.WritePage(40, buf));  // beyond DEFAULT_DB_IO_SIZE (16): grows the file
  }  // manager destroyed -> file closed

  {
    SingleFileDiskManager dm(path);  // reopen the same file
    data_t read_buf[PAGE_SIZE];
    ASSERT_TRUE(dm.ReadPage(0, read_buf));
    EXPECT_EQ(read_buf[0], 'a');
    ASSERT_TRUE(dm.ReadPage(3, read_buf));
    EXPECT_EQ(read_buf[0], 'd');
    ASSERT_TRUE(dm.ReadPage(40, read_buf)) << "a page in the grown region survives reopen";
    EXPECT_EQ(read_buf[0], 'z');
  }
  std::filesystem::remove(path);
}

TEST(MemoryDiskManagerTest, WriteThenReadRoundTrips) {
  MemoryDiskManager dm(8);
  data_t write_buf[PAGE_SIZE];
  data_t read_buf[PAGE_SIZE];
  FillPage(write_buf, 'M');
  ASSERT_TRUE(dm.WritePage(3, write_buf));
  ASSERT_TRUE(dm.ReadPage(3, read_buf));
  EXPECT_EQ(0, std::memcmp(write_buf, read_buf, PAGE_SIZE));
}

TEST(MemoryDiskManagerTest, ReadUnwrittenPageIsZeros) {
  MemoryDiskManager dm(8);
  data_t read_buf[PAGE_SIZE];
  FillPage(read_buf, 'Z');
  ASSERT_TRUE(dm.ReadPage(5, read_buf));
  data_t zero[PAGE_SIZE];
  FillPage(zero, 0);
  EXPECT_EQ(0, std::memcmp(read_buf, zero, PAGE_SIZE));
}

// Bug #5: an out-of-range page id must not read or write past the buffer (ASan proves no OOB).
TEST(MemoryDiskManagerTest, OutOfRangePageIsRejected) {
  MemoryDiskManager dm(4);  // valid ids: 0..3
  data_t buf[PAGE_SIZE];
  FillPage(buf, 'X');

  EXPECT_FALSE(dm.WritePage(4, buf));
  EXPECT_FALSE(dm.WritePage(-1, buf));

  data_t read_buf[PAGE_SIZE];
  FillPage(read_buf, 'X');
  EXPECT_FALSE(dm.ReadPage(4, read_buf));
  EXPECT_FALSE(dm.ReadPage(-1, read_buf));
  // The output buffer is defined (zero-filled) even on the rejected path.
  data_t zero[PAGE_SIZE];
  FillPage(zero, 0);
  EXPECT_EQ(0, std::memcmp(read_buf, zero, PAGE_SIZE));
}

}  // namespace bumblebee
