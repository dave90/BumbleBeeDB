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
#include <thread>  // NOLINT
#include <vector>

#include "common/config.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/disk/single_file_disk_manager.h"

namespace bumblebee {

static auto TempDbPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / name;
}

static void FillPage(data_ptr_t buf, data_t value) { std::memset(buf, value, PAGE_SIZE); }

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

// Regression: a db file grown past 2 GiB must still read back. GetFileSize returned a 32-bit int,
// which wraps once the file passes 2^31 bytes, and ReadPage read that as "file_size < 0" -> an I/O
// error, failing EVERY read beyond that point. Out-of-core operators spill into this file, so a spill
// larger than 2 GiB silently got zero-filled pages instead of its data (TPC-H q05 -> 0 rows, and an
// ever-growing multi-GiB file because writes kept succeeding). A page written past the boundary must
// round-trip. The file is sparse (resize_file), so this costs no real disk space.
TEST(SingleFileDiskManagerTest, ReadsPageBeyondTwoGigabytes) {
  auto path = TempDbPath("bbdb_dm_over_2gb.db");
  std::filesystem::remove(path);
  {
    SingleFileDiskManager dm(path);
    // A page whose byte offset lands past 2^31, so writing it grows the file beyond 2 GiB.
    const page_id_t page_id = static_cast<page_id_t>((int64_t{2} << 30) / PAGE_SIZE) + 16;
    ASSERT_GT(static_cast<int64_t>(page_id) * PAGE_SIZE, int64_t{2} << 30);

    data_t write_buf[PAGE_SIZE];
    data_t read_buf[PAGE_SIZE];
    FillPage(write_buf, 'Q');
    FillPage(read_buf, 0);
    ASSERT_TRUE(dm.WritePage(page_id, write_buf));
    ASSERT_GT(dm.GetDbFileSize(), static_cast<size_t>(int64_t{2} << 30)) << "file must have grown past 2 GiB";

    ASSERT_TRUE(dm.ReadPage(page_id, read_buf)) << "a read past 2 GiB must not be seen as an I/O error";
    EXPECT_EQ(0, std::memcmp(write_buf, read_buf, PAGE_SIZE)) << "a page beyond 2 GiB must round-trip intact";
  }
  std::filesystem::remove(path);
}

// Concurrent I/O through the SingleFileDiskManager: many threads write and read back their own disjoint
// pages at once. Writes to high page ids also race EnsureCapacity (file growth) — all serialized by the
// `db_io_latch_`, so no torn page, no lost write, and the counter reflects every write.
TEST(SingleFileDiskManagerTest, ConcurrentWritesAndReadsDisjointPages) {
  auto path = TempDbPath("bbdb_dm_concurrent.db");
  std::filesystem::remove(path);
  {
    SingleFileDiskManager dm(path);

    const int threads = 6;
    const int pages_per_thread = 20;  // 120 pages > DEFAULT_DB_IO_SIZE => concurrent file growth
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int t = 0; t < threads; t++) {
      workers.emplace_back([&, t] {
        for (int i = 0; i < pages_per_thread; i++) {
          const page_id_t page_id = t * pages_per_thread + i;
          data_t write_buf[PAGE_SIZE];
          FillPage(write_buf, static_cast<data_t>('A' + t));
          ASSERT_TRUE(dm.WritePage(page_id, write_buf));

          // Read our own page straight back (no other thread touches it) and verify integrity.
          data_t read_buf[PAGE_SIZE];
          ASSERT_TRUE(dm.ReadPage(page_id, read_buf));
          EXPECT_EQ(0, std::memcmp(write_buf, read_buf, PAGE_SIZE));
        }
      });
    }
    for (auto &w : workers) {
      w.join();
    }

    EXPECT_EQ(dm.GetNumWrites(), threads * pages_per_thread);

    // Final single-threaded pass: every page still holds exactly its owning thread's byte.
    for (int t = 0; t < threads; t++) {
      for (int i = 0; i < pages_per_thread; i++) {
        data_t read_buf[PAGE_SIZE];
        ASSERT_TRUE(dm.ReadPage(t * pages_per_thread + i, read_buf));
        EXPECT_EQ(read_buf[0], static_cast<data_t>('A' + t));
      }
    }
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
