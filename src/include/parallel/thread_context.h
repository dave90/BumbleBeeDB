//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// thread_context.h
//
// Identification: src/include/parallel/thread_context.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"
#include "parallel/thread_profiler.h"

namespace bumblebee {

/**
 * @brief The state of exactly one task (one worker thread draining one pipeline).
 *
 * Because it is single-threaded by construction, nothing here needs a lock — that is the whole reason
 * it exists. It owns the task's `ThreadProfiler`; a future per-task memory-reservation handle / spill
 * arena would live here too. Note the direction: a `ThreadContext` does NOT own a `ClientContext`
 * (BumbleBee had that backwards); the two are bundled by an `ExecutionContext` instead.
 */
class ThreadContext {
 public:
  explicit ThreadContext(idx_t num_ops) : profiler_(num_ops) {}

  ThreadProfiler profiler_;
};

}  // namespace bumblebee
