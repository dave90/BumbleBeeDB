//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_sort.h
//
// Identification: src/include/execution/operator/order/physical_sort.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "binder/bound_order_by.h"
#include "execution/physical_operator.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

/**
 * @brief A blocking sort (sink + source): materialize all input, sort by the ORDER BY keys, re-emit.
 *
 * Keys are turned into memcmp-comparable byte strings via `CreateSortKey`, so the sort is a single
 * comparison of encoded keys. Order-preserving, so its source pipeline runs single-threaded.
 */
class PhysicalSort : public PhysicalOperator {
 public:
  PhysicalSort(SchemaRef output_schema, std::vector<OrderBy> order_bys, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::SORT, std::move(output_schema), child->estimated_cardinality_),
        order_bys_(std::move(order_bys)) {
    for (const auto &ob : order_bys_) {
      const auto type = std::get<0>(ob);
      modifiers_.emplace_back(type == OrderByType::DESC ? OrderType::DESCENDING : OrderType::ASCENDING);
    }
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
  auto IsOrderPreserving() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  std::vector<OrderBy> order_bys_;
  std::vector<OrderModifiers> modifiers_;
};

}  // namespace bumblebee
