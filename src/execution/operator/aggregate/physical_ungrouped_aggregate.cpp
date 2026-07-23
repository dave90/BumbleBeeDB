//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_ungrouped_aggregate.cpp
//
// Identification: src/execution/operator/aggregate/physical_ungrouped_aggregate.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/aggregate/physical_ungrouped_aggregate.h"

#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <vector>

#include "execution/aggregate/aggregate_state.h"
#include "execution/expression_executor.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"

namespace bumblebee {

struct UngroupedGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  std::vector<AggregateAccumulator> accums_;
};

struct UngroupedLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> exec_;
  std::vector<LogicalType> arg_types_;
  std::vector<AggregateAccumulator> accums_;
  /** Reused per Sink call: Execute() re-references its columns, so no per-chunk allocation. */
  DataChunk args_;
};

struct UngroupedGlobalSourceState : GlobalSourceState {
  GlobalSinkState *sink_{nullptr};
  std::atomic<bool> emitted_{false};
  auto MaxThreads() -> idx_t override { return 1; }
};

auto PhysicalUngroupedAggregate::GetGlobalSinkState(ClientContext & /*context*/) const
    -> std::unique_ptr<GlobalSinkState> {
  auto gs = std::make_unique<UngroupedGlobalSinkState>();
  for (auto type : agg_types_) {
    gs->accums_.emplace_back(type);
  }
  return gs;
}

auto PhysicalUngroupedAggregate::GetLocalSinkState(ExecutionContext & /*context*/) const
    -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<UngroupedLocalSinkState>();
  ls->exec_ = std::make_unique<ExpressionExecutor>();
  for (const auto &agg : aggregates_) {
    ls->exec_->AddExpression(*agg);
    ls->arg_types_.push_back(agg->GetReturnType().GetType());
  }
  for (auto type : agg_types_) {
    ls->accums_.emplace_back(type);
  }
  ls->args_.Initialize(ls->arg_types_);
  return ls;
}

auto PhysicalUngroupedAggregate::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                      LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<UngroupedLocalSinkState &>(lstate);
  ls.exec_->Execute(input, ls.args_);
  // Column-at-a-time: each aggregate folds its whole argument vector in one pass over contiguous
  // data (constants collapse to O(1)) instead of a row loop boxing every cell into a Value.
  const idx_t count = input.GetSize();
  for (idx_t a = 0; a < ls.accums_.size(); a++) {
    ls.accums_[a].UpdateVector(ls.args_.data_[a], count);
  }
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalUngroupedAggregate::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                         LocalSinkState &lstate) const {
  auto &gs = static_cast<UngroupedGlobalSinkState &>(gstate);
  auto &ls = static_cast<UngroupedLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  for (idx_t a = 0; a < gs.accums_.size(); a++) {
    gs.accums_[a].Merge(ls.accums_[a]);
  }
}

auto PhysicalUngroupedAggregate::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<UngroupedGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalUngroupedAggregate::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                         LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<UngroupedGlobalSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;
  }
  auto &sink = *static_cast<UngroupedGlobalSinkState *>(src.sink_);
  for (idx_t a = 0; a < sink.accums_.size(); a++) {
    output.SetValue(a, 0, sink.accums_[a].Finalize(output_schema_->GetColumn(a).GetType()));
  }
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalUngroupedAggregate::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
