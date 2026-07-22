//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// pipeline_executor.h
//
// Identification: src/include/parallel/pipeline_executor.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "execution/execution_context.h"
#include "execution/physical_operator.h"
#include "parallel/pipeline.h"
#include "parallel/thread_context.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/**
 * @brief One task's drain of one pipeline — owns every piece of thread-local state and the push loop.
 *
 * There is exactly one per (task, pipeline). Chunks and local states are allocated once at construction
 * and `Reset()` in the loop, so the hot path never allocates. The push loop keeps one cached chunk
 * *per operator boundary* (so a re-entrant operator's parked cursor into its input chunk stays intact),
 * and a LIFO of operators that still owe output, drained highest index first.
 */
class PipelineExecutor {
 public:
  PipelineExecutor(Pipeline &pipeline, ClientContext &client, ThreadContext &thread);

  /** @brief Drain the source through the operators into the sink until finished; then Combine. */
  void Run();

 private:
  auto FetchFromSource(DataChunk &result) -> SourceResultType;
  auto ExecutePushInternal(DataChunk &input, idx_t initial_idx = 0) -> OperatorResultType;
  auto Execute(DataChunk &input, DataChunk &result, idx_t initial_idx) -> OperatorResultType;
  auto Sink(DataChunk &chunk) -> SinkResultType;
  void PushFinalize();
  void GoToSource(idx_t &current_idx, idx_t initial_idx);

  Pipeline &pipeline_;
  ExecutionContext context_;

  std::unique_ptr<LocalSourceState> local_source_state_;
  std::vector<std::unique_ptr<LocalOperatorState>> intermediate_states_;
  std::unique_ptr<LocalSinkState> local_sink_state_;

  /** One cached chunk per boundary: [0] = source output, [i] = output of operators_[i-1]. */
  std::vector<std::unique_ptr<DataChunk>> intermediate_chunks_;
  DataChunk final_chunk_;  // the chunk handed to the sink (last operator's output)

  /** LIFO of operators still holding buffered output. Invariant: strictly increasing bottom→top. */
  std::vector<idx_t> in_process_operators_;
  bool exhausted_source_{false};
  bool finished_{false};
  bool finalized_{false};
};

}  // namespace bumblebee
