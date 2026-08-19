//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// disk_scheduler_test.cpp
//
// Identification: test/unit/storage/disk/disk_scheduler_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/disk/disk_scheduler.h"

#include <cstring>
#include <future>  // NOLINT
#include <stdexcept>
#include <thread>  // NOLINT
#include <vector>

#include "common/config.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"

namespace bumblebee {

/** @brief A disk manager whose WritePage always throws. */
class ThrowingDiskManager : public DiskManager {
 public:
  auto WritePage(page_id_t /*page_id*/, const_data_ptr_t /*page_data*/) -> bool override {
    throw std::runtime_error("simulated write failure");
  }
  auto ReadPage(page_id_t /*page_id*/, data_ptr_t page_data) -> bool override {
    std::memset(page_data, 0, PAGE_SIZE);
    return true;
  }
  auto DeletePage(page_id_t /*page_id*/) -> void override {}
};

/** @brief A disk manager whose WritePage always reports failure (returns false). */
class FailingDiskManager : public DiskManager {
 public:
  auto WritePage(page_id_t /*page_id*/, const_data_ptr_t /*page_data*/) -> bool override { return false; }
  auto ReadPage(page_id_t /*page_id*/, data_ptr_t page_data) -> bool override {
    std::memset(page_data, 0, PAGE_SIZE);
    return true;
  }
  auto DeletePage(page_id_t /*page_id*/) -> void override {}
};

static auto Submit(DiskScheduler &scheduler, bool is_write, page_id_t page_id, data_ptr_t data) -> std::future<bool> {
  DiskRequest request{is_write, data, page_id, scheduler.CreatePromise()};
  auto future = request.callback_.get_future();
  scheduler.Schedule(request);
  return future;
}

// Many client threads submit reads/writes to disjoint pages at once. The DiskScheduler funnels them
// through one MPMC channel drained by several workers; every future must resolve and each page must
// read back exactly what its owning thread wrote (concurrent Schedule() + concurrent worker I/O).
TEST(DiskSchedulerTest, ConcurrentClientsDisjointPages) {
  const int threads = 8;
  const int pages_per_thread = 16;
  MemoryDiskManager dm(threads * pages_per_thread + 1);
  DiskScheduler scheduler(&dm);

  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (int t = 0; t < threads; t++) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < pages_per_thread; i++) {
        const page_id_t page_id = t * pages_per_thread + i;
        data_t write_buf[PAGE_SIZE];
        std::memset(write_buf, static_cast<int>('A' + t), PAGE_SIZE);
        ASSERT_TRUE(Submit(scheduler, /*is_write=*/true, page_id, write_buf).get());

        data_t read_buf[PAGE_SIZE];
        ASSERT_TRUE(Submit(scheduler, /*is_write=*/false, page_id, read_buf).get());
        EXPECT_EQ(0, std::memcmp(write_buf, read_buf, PAGE_SIZE));
      }
    });
  }
  for (auto &w : workers) {
    w.join();
  }
}

TEST(DiskSchedulerTest, ReadWriteRoundTrip) {
  MemoryDiskManager dm(8);
  DiskScheduler scheduler(&dm);

  data_t write_buf[PAGE_SIZE];
  std::memset(write_buf, 'S', PAGE_SIZE);
  ASSERT_TRUE(Submit(scheduler, /*is_write=*/true, 2, write_buf).get());

  data_t read_buf[PAGE_SIZE];
  ASSERT_TRUE(Submit(scheduler, /*is_write=*/false, 2, read_buf).get());
  EXPECT_EQ(0, std::memcmp(write_buf, read_buf, PAGE_SIZE));
}

// Bug #3a: a throwing disk op must resolve the future (as an exception) — not deadlock — and the
// worker must survive so subsequent requests still complete.
TEST(DiskSchedulerTest, ThrowingOpDoesNotDeadlockWorker) {
  ThrowingDiskManager dm;
  DiskScheduler scheduler(&dm);

  data_t buf[PAGE_SIZE];
  std::memset(buf, 'T', PAGE_SIZE);
  auto bad = Submit(scheduler, /*is_write=*/true, 0, buf);
  EXPECT_THROW(bad.get(), std::runtime_error);

  // The worker is still alive: a following read completes.
  data_t read_buf[PAGE_SIZE];
  EXPECT_TRUE(Submit(scheduler, /*is_write=*/false, 0, read_buf).get());
}

// Bug #3b: a failing disk op resolves the future with false, not a hang.
TEST(DiskSchedulerTest, FailingOpResolvesFalse) {
  FailingDiskManager dm;
  DiskScheduler scheduler(&dm);

  data_t buf[PAGE_SIZE];
  std::memset(buf, 'F', PAGE_SIZE);
  EXPECT_FALSE(Submit(scheduler, /*is_write=*/true, 0, buf).get());
}

}  // namespace bumblebee
