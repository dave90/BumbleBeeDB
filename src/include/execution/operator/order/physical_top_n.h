//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_top_n.h
//
// Identification: src/include/execution/operator/order/physical_top_n.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "binder/bound_order_by.h"
#include "execution/physical_operator.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

class TopNHeap;

/**
 * @brief ORDER BY ... LIMIT n collapsed into one operator: a bounded heap keeps only the top n rows.
 *
 * Never spills — memory is bounded by n. Sink + source: the sink maintains an n-bounded max-heap keyed
 * by the encoded ORDER BY key (evicting the current worst when a better row arrives); the source emits
 * the survivors in sorted order.
 */
class PhysicalTopN : public PhysicalOperator {
 public:
  PhysicalTopN(SchemaRef output_schema, std::vector<OrderBy> order_bys, std::size_t n,
               std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::TOP_N, std::move(output_schema), n), order_bys_(std::move(order_bys)),
        n_(n) {
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

  auto ParamsToString() const -> std::string override { return "{ n=" + std::to_string(n_) + " }"; }

  /** @brief A TopNHeap sized and keyed for this operator. */
  auto MakeHeap() const -> std::unique_ptr<TopNHeap>;

  std::vector<OrderBy> order_bys_;
  std::vector<OrderModifiers> modifiers_;
  std::size_t n_;
};

}  // namespace bumblebee
