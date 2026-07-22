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
 * @brief A GROUP BY aggregate over a hash table keyed by the encoded group-by columns.
 *
 * Sink + source: the sink hashes each row into a per-group accumulator set; the source emits one row per
 * distinct group (the group-by columns, then the finalized aggregates). Single-threaded map for now; the
 * radix-partitioned lock-free table is a parallel-scheduler-era optimization.
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
