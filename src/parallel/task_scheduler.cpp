//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// task_scheduler.cpp
//
// Identification: src/parallel/task_scheduler.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "parallel/task_scheduler.h"

#include <utility>

namespace bumblebee {

void TaskScheduler::EnqueueAll(std::vector<TaskRef> tasks) {
  if (tasks.empty()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    for (auto &t : tasks) {
      queue_.push_back(std::move(t));
    }
  }
  cv_.notify_all();
}

void TaskScheduler::WorkUntil(std::atomic<bool> &done) {
  while (true) {
    TaskRef task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return !queue_.empty() || done.load(std::memory_order_acquire); });
      if (queue_.empty()) {
        return;  // woken with an empty queue and `done` set: this worker is finished
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task->Execute();  // the lock is RELEASED first — a task's finalize re-enters EnqueueAll
  }
}

void TaskScheduler::NotifyAll() {
  // Acquire the mutex before notifying. The `done` flag this wakes workers for is set outside the
  // scheduler lock, so notifying without it risks a lost wakeup: a worker that read `done == false`
  // under the lock could then sleep in cv_.wait *after* an unlocked notify fired, and hang forever.
  // Taking the lock serializes this notify against that check-then-sleep window.
  std::lock_guard lock(mutex_);
  cv_.notify_all();
}

}  // namespace bumblebee
