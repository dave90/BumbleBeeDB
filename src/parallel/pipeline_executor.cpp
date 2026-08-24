//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// pipeline_executor.cpp
//
// Identification: src/parallel/pipeline_executor.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "parallel/pipeline_executor.h"

#include <memory>
#include <vector>

#include "common/macros.h"
#include "parallel/executor.h"
#include "parallel/thread_profiler.h"

namespace bumblebee {

/** @brief A fresh chunk initialized to `op`'s output column types. */
static auto MakeChunkFor(const PhysicalOperator &op) -> std::unique_ptr<DataChunk> {
  auto chunk = std::make_unique<DataChunk>();
  chunk->Initialize(op.output_schema_->GetTypes());
  return chunk;
}

PipelineExecutor::PipelineExecutor(Pipeline &pipeline, ClientContext &client, ThreadContext &thread)
    : pipeline_(pipeline), context_(client, thread, &pipeline) {
  // Source-side local state.
  local_source_state_ = pipeline_.source_->GetLocalSourceState(context_, *pipeline_.source_gstate_);

  // Per-operator local state.
  intermediate_states_.reserve(pipeline_.operators_.size());
  for (auto *op : pipeline_.operators_) {
    intermediate_states_.push_back(op->GetLocalOperatorState(context_));
  }

  // Sink-side local state (every pipeline ends in a sink).
  local_sink_state_ = pipeline_.sink_->GetLocalSinkState(context_);

  // One cached chunk per boundary: [0] = source output, [i] = output of operators_[i-1].
  const idx_t num_chunks = std::max<idx_t>(1, pipeline_.operators_.size());
  intermediate_chunks_.reserve(num_chunks);
  intermediate_chunks_.push_back(MakeChunkFor(*pipeline_.source_));
  for (idx_t i = 1; i < pipeline_.operators_.size(); i++) {
    intermediate_chunks_.push_back(MakeChunkFor(*pipeline_.operators_[i - 1]));
  }

  // The chunk handed to the sink is the last operator's output (or the source's, if no operators).
  if (pipeline_.operators_.empty()) {
    final_chunk_.Initialize(pipeline_.source_->output_schema_->GetTypes());
  } else {
    final_chunk_.Initialize(pipeline_.operators_.back()->output_schema_->GetTypes());
  }

  // Written-columns hint per boundary: the source's, carried through column-preserving operators.
  chunk_written_.resize(pipeline_.operators_.size() + 1);
  chunk_written_[0] = pipeline_.source_->SourceWrittenColumns();
  for (idx_t i = 0; i < pipeline_.operators_.size(); i++) {
    chunk_written_[i + 1] = pipeline_.operators_[i]->PreservesInputColumns() ? chunk_written_[i] : nullptr;
  }
}

void PipelineExecutor::GoToSource(idx_t &current_idx, idx_t initial_idx) {
  if (!in_process_operators_.empty()) {
    current_idx = in_process_operators_.back();
    in_process_operators_.pop_back();
  } else {
    current_idx = initial_idx;
  }
}

auto PipelineExecutor::FetchFromSource(DataChunk &result) -> SourceResultType {
  if (pipeline_.dead_.load(std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;  // an upstream NO_OUTPUT_POSSIBLE: never open the source
  }
  ProfileScope scope(context_.thread_.profiler_, pipeline_.source_->id_, Phase::Source);
  auto res = pipeline_.source_->GetData(context_, result, *pipeline_.source_gstate_, *local_source_state_);
  auto &prof = context_.thread_.profiler_[pipeline_.source_->id_];
  prof.rows_out += result.GetSize();
  prof.chunks += 1;
  return res;
}

auto PipelineExecutor::Sink(DataChunk &chunk) -> SinkResultType {
  ProfileScope scope(context_.thread_.profiler_, pipeline_.sink_->id_, Phase::Sink);
  context_.thread_.profiler_[pipeline_.sink_->id_].rows_in += chunk.GetSize();
  return pipeline_.sink_->Sink(context_, chunk, *pipeline_.sink_gstate_, *local_sink_state_);
}

auto PipelineExecutor::Execute(DataChunk &input, DataChunk &result, idx_t initial_idx) -> OperatorResultType {
  idx_t current_idx = 0;
  GoToSource(current_idx, initial_idx);
  if (current_idx == initial_idx) {
    current_idx++;
  }

  while (current_idx <= pipeline_.operators_.size()) {
    auto &prev_chunk = (current_idx == initial_idx + 1) ? input : *intermediate_chunks_[current_idx - 1];
    const auto &current_op = *pipeline_.operators_[current_idx - 1];
    auto &current_chunk = (current_idx == pipeline_.operators_.size()) ? result : *intermediate_chunks_[current_idx];

    ResetChunk(current_chunk, current_idx);
    OperatorResultType res;
    {
      ProfileScope scope(context_.thread_.profiler_, current_op.id_, Phase::Execute);
      res = current_op.Execute(context_, prev_chunk, current_chunk, *pipeline_.operator_gstates_[current_idx - 1],
                               *intermediate_states_[current_idx - 1]);
    }
    auto &prof = context_.thread_.profiler_[current_op.id_];
    prof.rows_in += prev_chunk.GetSize();
    prof.rows_out += current_chunk.GetSize();

    if (res == OperatorResultType::FINISHED) {
      return OperatorResultType::FINISHED;
    }
    if (res == OperatorResultType::HAVE_MORE_OUTPUT) {
      BUMBLEBEE_ASSERT(current_chunk.GetSize() > 0, "HAVE_MORE_OUTPUT with an empty chunk loops forever");
      in_process_operators_.push_back(current_idx);
    }
    if (current_chunk.GetSize() == 0) {
      // This operator swallowed everything (a filter that matched nothing). Nothing to push down.
      if (in_process_operators_.empty()) {
        return OperatorResultType::NEED_MORE_INPUT;
      }
      GoToSource(current_idx, initial_idx);  // resume whoever still owes output
      continue;
    }
    current_idx++;
  }
  return in_process_operators_.empty() ? OperatorResultType::NEED_MORE_INPUT : OperatorResultType::HAVE_MORE_OUTPUT;
}

auto PipelineExecutor::ExecutePushInternal(DataChunk &input, idx_t initial_idx) -> OperatorResultType {
  if (pipeline_.operators_.empty()) {
    return Sink(input) == SinkResultType::FINISHED ? OperatorResultType::FINISHED : OperatorResultType::NEED_MORE_INPUT;
  }
  while (true) {
    ResetChunk(final_chunk_, pipeline_.operators_.size());
    auto result = Execute(input, final_chunk_, initial_idx);
    if (result == OperatorResultType::FINISHED) {
      return OperatorResultType::FINISHED;
    }
    if (final_chunk_.GetSize() > 0 && Sink(final_chunk_) == SinkResultType::FINISHED) {
      return OperatorResultType::FINISHED;
    }
    if (result == OperatorResultType::NEED_MORE_INPUT) {
      return OperatorResultType::NEED_MORE_INPUT;
    }
    // HAVE_MORE_OUTPUT: the chain still owes output for THIS input. Re-enter; do not refetch.
  }
}

void PipelineExecutor::Run() {
  auto &source_chunk = *intermediate_chunks_[0];
  while (!finished_) {
    if (context_.client_.txn_ != nullptr) {
      context_.client_.txn_->ThrowIfCancellationRequested();
    }
    if (pipeline_.stop_.load(std::memory_order_relaxed) || pipeline_.executor_.HasError()) {
      break;  // cancellation latency is bounded by one chunk
    }
    if (in_process_operators_.empty()) {
      if (exhausted_source_) {
        break;
      }
      ResetChunk(source_chunk, 0);
      if (FetchFromSource(source_chunk) == SourceResultType::FINISHED) {
        exhausted_source_ = true;
      }
      if (source_chunk.GetSize() == 0) {
        continue;  // the loop head breaks if also exhausted
      }
      // Every chunk derived from this source chunk inherits its batch index (streaming operators
      // stay within the task), so an order-dependent sink can restore the serial order.
      local_sink_state_->batch_idx_ = local_source_state_->batch_idx_;
    }
    if (ExecutePushInternal(source_chunk) == OperatorResultType::FINISHED) {
      finished_ = true;
    }
  }
  if (context_.client_.txn_ != nullptr) {
    context_.client_.txn_->ThrowIfCancellationRequested();
  }
  PushFinalize();  // Combine — runs even on an early LIMIT exit
}

void PipelineExecutor::PushFinalize() {
  if (finalized_) {
    return;
  }
  finalized_ = true;
  ProfileScope scope(context_.thread_.profiler_, pipeline_.sink_->id_, Phase::Combine);
  pipeline_.sink_->Combine(context_, *pipeline_.sink_gstate_, *local_sink_state_);
}

}  // namespace bumblebee
