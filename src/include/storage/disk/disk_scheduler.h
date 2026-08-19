//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// disk_scheduler.h
//
// Identification: src/include/storage/disk/disk_scheduler.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <future>  // NOLINT
#include <optional>
#include <thread>  // NOLINT
#include <vector>

#include "storage/common/channel.h"
#include "storage/disk/disk_manager.h"

namespace bumblebee {

/**
 * @brief A single read or write for the disk manager to execute asynchronously.
 */
struct DiskRequest {
  /** True for a write, false for a read. */
  bool is_write_;

  /** The buffer to write from (write) or read into (read); points into a buffer-pool frame. */
  data_ptr_t data_;

  /** The page being read or written. */
  page_id_t page_id_;

  /** Resolved when the request completes: true on success, false on I/O error. */
  std::promise<bool> callback_;
};

/**
 * @brief Runs disk reads and writes on a pool of background worker threads.
 *
 * A request is submitted with `Schedule`; the caller keeps the request's future and blocks on it when
 * it needs the result. Workers drain a shared MPMC FIFO queue, so independent-page I/Os run in
 * parallel. Ordering between two requests to the *same* page is therefore not guaranteed across
 * workers — the buffer pool relies on awaiting each request before the page's frame is reused, so no
 * two requests for one page are ever in flight at once. The disk backend must be safe for concurrent
 * requests to distinct pages.
 */
class DiskScheduler {
 public:
  explicit DiskScheduler(DiskManager *disk_manager, size_t num_workers = DISK_SCHEDULER_WORKER_COUNT);
  ~DiskScheduler();

  DiskScheduler(const DiskScheduler &) = delete;
  auto operator=(const DiskScheduler &) -> DiskScheduler & = delete;
  DiskScheduler(DiskScheduler &&) = delete;
  auto operator=(DiskScheduler &&) -> DiskScheduler & = delete;

  /** @brief Schedule a batch of requests, in order. */
  void Schedule(std::vector<DiskRequest> &requests);

  /** @brief Schedule a single request. */
  void Schedule(DiskRequest &request);

  /** @brief The worker loop. Runs until a stop sentinel is enqueued by the destructor. */
  void StartWorkerThread();

  using DiskSchedulerPromise = std::promise<bool>;

  /** @brief Create a fresh promise for a request's callback. */
  auto CreatePromise() -> DiskSchedulerPromise { return {}; }

  /** @brief Deallocate a page on disk. */
  void DeallocatePage(page_id_t page_id) { disk_manager_->DeletePage(page_id); }

 private:
  /** The disk manager requests are issued against. */
  DiskManager *disk_manager_;

  /** The request queue. A `std::nullopt` sentinel tells the worker to stop. */
  Channel<std::optional<DiskRequest>> request_queue_;

  /** The pool of background workers, all draining `request_queue_`. */
  std::vector<std::thread> workers_;
};

}  // namespace bumblebee
