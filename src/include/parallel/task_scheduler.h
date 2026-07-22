//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// task_scheduler.h
//
// Identification: src/include/parallel/task_scheduler.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <condition_variable>  // NOLINT
#include <deque>
#include <mutex>  // NOLINT
#include <vector>

#include "parallel/task.h"

namespace bumblebee {

/**
 * @brief A mutex + condition-variable task deque. Tasks are coarse (one full pipeline drain each), so
 * the queue is touched once per task, not once per chunk.
 *
 * The client thread joins the worker set via `WorkUntil` rather than idling a core on a coordinator.
 * There is no polling and no barrier — completion is a counter the last task drives.
 */
class TaskScheduler {
 public:
  TaskScheduler() = default;

  /** @brief Publish tasks under one lock and wake every worker. */
  void EnqueueAll(std::vector<TaskRef> tasks);

  /**
   * @brief Run tasks until the queue is empty AND `done` is set (or shutdown).
   *
   * The lock is released before a task executes — load-bearing, because a task's finalize re-enters
   * `EnqueueAll` (hazard H2).
   */
  void WorkUntil(std::atomic<bool> &done);

  /** @brief Wake every waiting worker (e.g. after the last task sets the done flag — hazard H1). */
  void NotifyAll();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<TaskRef> queue_;
};

}  // namespace bumblebee
