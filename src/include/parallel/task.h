//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// task.h
//
// Identification: src/include/parallel/task.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>

#include "parallel/thread_context.h"

namespace bumblebee {

class Executor;
class Pipeline;

/** @brief A unit of scheduled work: one thread's entire drain of one pipeline. */
class Task {
 public:
  virtual ~Task() = default;
  virtual void Execute() = 0;

  /** The query this task belongs to — a seam for a future multi-session worker pool (hazard H3). */
  Executor *owner_{nullptr};
};

using TaskRef = std::unique_ptr<Task>;

/**
 * @brief One task over one pipeline: run a `PipelineExecutor`, then the last-task-wins finalize protocol.
 *
 * Owns its own `ThreadContext` (single-threaded by construction), so its profiler needs no lock; the
 * `Executor` merges it after the drain.
 */
class PipelineTask : public Task {
 public:
  PipelineTask(Executor &executor, Pipeline &pipeline, idx_t num_operators)
      : executor_(executor), pipeline_(pipeline), thread_(num_operators) {
    owner_ = &executor;
  }

  void Execute() override;

 private:
  Executor &executor_;
  Pipeline &pipeline_;
  ThreadContext thread_;
};

}  // namespace bumblebee
