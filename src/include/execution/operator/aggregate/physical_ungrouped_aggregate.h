//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_ungrouped_aggregate.h
//
// Identification: src/include/execution/operator/aggregate/physical_ungrouped_aggregate.h
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

namespace bumblebee {

/**
 * @brief An aggregate with no GROUP BY (`SELECT COUNT(*), SUM(x) FROM t`) — always exactly one output row.
 *
 * Sink + source: the sink folds every input row into one accumulator per aggregate; the source emits the
 * single finalized row. Emits its row even on empty input (COUNT(*) = 0), which is why an empty-source
 * pipeline still gets a task.
 */
class PhysicalUngroupedAggregate : public PhysicalOperator {
 public:
  PhysicalUngroupedAggregate(SchemaRef output_schema, std::vector<AbstractExpressionRef> aggregates,
                             std::vector<AggregationType> agg_types, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::UNGROUPED_AGGREGATE, std::move(output_schema), 1),
        aggregates_(std::move(aggregates)),
        agg_types_(std::move(agg_types)) {
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;

  auto IsSource() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  std::vector<AbstractExpressionRef> aggregates_;
  std::vector<AggregationType> agg_types_;
};

}  // namespace bumblebee
