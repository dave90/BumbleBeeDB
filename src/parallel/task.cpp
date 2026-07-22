//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// task.cpp
//
// Identification: src/parallel/task.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "parallel/task.h"

#include <exception>

#include "parallel/executor.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_executor.h"

namespace bumblebee {

void PipelineTask::Execute() {
  try {
    if (!executor_.HasError()) {
      PipelineExecutor pipe_executor(pipeline_, executor_.Context(), thread_);
      pipe_executor.Run();  // drains the source through the operators into the sink, then Combine
    }
  } catch (...) {
    executor_.PushError(std::current_exception());  // latches the FIRST exception
  }

  // (A) LAST-TASK-WINS. Must precede (B) — rule R1: any tasks this one enqueues must be counted in
  // active_tasks_ before this task decrements its own, or the count could transiently hit zero.
  if (pipeline_.tasks_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (executor_.HasError()) {
      executor_.AbortPipeline(pipeline_);  // count it done; release no dependents
    } else {
      try {
        executor_.RunFinalize(pipeline_);  // may enqueue dependents' tasks
      } catch (...) {
        executor_.PushError(std::current_exception());  // a half-built sink state: fail the query
      }
    }
  }

  // Fold this task's profile in before the query can be declared over (the last task's TaskFinished
  // sets query_done_, and every earlier task already merged before its own TaskFinished).
  executor_.MergeThreadProfiler(thread_.profiler_);

  // (B) global bookkeeping.
  executor_.TaskFinished();
}

}  // namespace bumblebee
