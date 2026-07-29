//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_result_collector.h
//
// Identification: src/include/execution/operator/helper/physical_result_collector.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "execution/physical_operator.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/** @brief The materialized result of a query: the chunks a `PhysicalResultCollector` gathered. */
class ResultCollectorGlobalState : public GlobalSinkState {
 public:
  std::mutex mu_;
  std::vector<std::unique_ptr<DataChunk>> chunks_;
  /** Chunks as combined, each tagged with its source batch index; Finalize stable-sorts them by
   * batch into `chunks_`, reconstructing the serial scan order from a parallel pipeline. */
  std::vector<std::pair<idx_t, std::unique_ptr<DataChunk>>> tagged_;

  /** @return The total number of result rows across all chunks. */
  auto RowCount() const -> idx_t {
    idx_t n = 0;
    for (const auto &c : chunks_) {
      n += c->GetSize();
    }
    return n;
  }
};

/** @brief One worker's private buffer of result chunks, moved into the global set at Combine. */
class ResultCollectorLocalState : public LocalSinkState {
 public:
  std::vector<std::pair<idx_t, std::unique_ptr<DataChunk>>> chunks_;
};

/**
 * @brief The root sink of every SELECT: it gathers the query's output chunks with zero row copies at
 * Combine (the local chunks are `std::move`d into the global set).
 */
class PhysicalResultCollector : public PhysicalOperator {
 public:
  explicit PhysicalResultCollector(std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::RESULT_COLLECTOR, child->output_schema_, child->estimated_cardinality_) {
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override {
    current.sink_ = this;
    children_[0]->BuildPipelines(current, builder);
  }

  auto GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> override {
    return std::make_unique<ResultCollectorGlobalState>();
  }
  auto GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> override {
    return std::make_unique<ResultCollectorLocalState>();
  }

  auto Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
            LocalSinkState &lstate) const -> SinkResultType override {
    // The pipeline reuses its chunks, so take an owned copy of this batch (the one copy at a breaker).
    // Clone only *references* the source columns, and a filtered batch arrives as a dictionary over the
    // filter's reused selection vector; without flattening it here, a later batch that overwrites that
    // selection would retroactively corrupt this stored batch (it would read the wrong rows). Normalify
    // materializes every column into owned flat storage so each stored batch is independent.
    auto &ls = static_cast<ResultCollectorLocalState &>(lstate);
    auto owned = input.Clone();
    owned->Normalify();
    ls.chunks_.emplace_back(ls.batch_idx_, std::move(owned));
    return SinkResultType::NEED_MORE_INPUT;
  }

  void Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate, LocalSinkState &lstate) const override {
    auto &gs = static_cast<ResultCollectorGlobalState &>(gstate);
    auto &ls = static_cast<ResultCollectorLocalState &>(lstate);
    std::lock_guard lock(gs.mu_);
    for (auto &c : ls.chunks_) {
      gs.tagged_.push_back(std::move(c));  // zero row copies: just move the unique_ptr
    }
    ls.chunks_.clear();
  }

  auto Finalize(ClientContext & /*context*/, GlobalSinkState &gstate, idx_t /*stage*/, idx_t /*task_idx*/,
                idx_t /*task_count*/) const -> SinkFinalizeType override {
    // One morsel is drained by exactly one task, so chunks sharing a batch index arrived in order
    // within one local buffer; the stable sort therefore reconstructs the full serial order.
    auto &gs = static_cast<ResultCollectorGlobalState &>(gstate);
    std::stable_sort(gs.tagged_.begin(), gs.tagged_.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    gs.chunks_.reserve(gs.tagged_.size());
    for (auto &t : gs.tagged_) {
      gs.chunks_.push_back(std::move(t.second));
    }
    gs.tagged_.clear();
    return SinkFinalizeType::READY;
  }

  // The collector may run in parallel: chunks are tagged with their source batch index and Finalize
  // restores the serial order. Pipeline::MaxThreads still serializes it when the source cannot
  // provide batch indexes (SinkOrderDependent + !SourceProvidesBatchIndex).
  auto SinkOrderDependent() const -> bool override { return true; }
};

}  // namespace bumblebee
