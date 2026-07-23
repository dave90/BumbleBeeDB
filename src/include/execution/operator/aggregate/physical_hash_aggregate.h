//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_hash_aggregate.h
//
// Identification: src/include/execution/operator/aggregate/physical_hash_aggregate.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/physical_operator.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

/**
 * @brief A GROUP BY aggregate over a hash table keyed by the group-by columns.
 *
 * Sink + source. Each sink task builds a thread-local table whose rows embed the aggregate state
 * next to the group key (per aggregate: a count and a value slot — no per-group heap allocations),
 * updated by columnar kernels. `Combine` does NOT merge into one global table under one lock:
 * it scatters each local table's rows into hash-partitioned row buffers (one small mutex per
 * partition), and the SOURCE then builds and emits one partition per task — so the merge, the
 * usual serial bottleneck of high-cardinality GROUP BYs, runs in parallel across partitions.
 */
class PhysicalHashAggregate : public PhysicalOperator {
 public:
  PhysicalHashAggregate(SchemaRef output_schema, std::vector<AbstractExpressionRef> group_bys,
                        std::vector<AbstractExpressionRef> aggregates, std::vector<AggregationType> agg_types,
                        std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::HASH_AGGREGATE, std::move(output_schema),
                         child->estimated_cardinality_),
        group_bys_(std::move(group_bys)),
        aggregates_(std::move(aggregates)),
        agg_types_(std::move(agg_types)) {
    group_modifiers_.assign(group_bys_.size(), OrderModifiers(OrderType::ASCENDING));
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;
  auto Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t stage, idx_t task_idx, idx_t task_count) const
      -> SinkFinalizeType override;

  auto IsSource() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const
      -> std::unique_ptr<LocalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  auto ParamsToString() const -> std::string override {
    return "{ groups=" + std::to_string(group_bys_.size()) + ", aggs=" + std::to_string(agg_types_.size()) + " }";
  }

  std::vector<AbstractExpressionRef> group_bys_;
  std::vector<AbstractExpressionRef> aggregates_;
  std::vector<AggregationType> agg_types_;
  std::vector<OrderModifiers> group_modifiers_;
};

}  // namespace bumblebee
