//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_external_merge_sort.h
//
// Identification: src/include/execution/operator/order/physical_external_merge_sort.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "binder/bound_order_by.h"
#include "catalog/schema.h"
#include "execution/physical_operator.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

/**
 * @brief The out-of-core sort: generate sorted runs on spill pages, then k-way merge them.
 *
 * Sink + source, same external contract as `PhysicalSort` (identical rows, identical order). The sink
 * buffers input until the query memory budget is hit, sorts that batch, and flushes it as a **run** — a
 * `SpillCollection` of `[encoded-key, columns...]` rows in sorted order. The source (single-threaded,
 * order-preserving) merges all runs with a min-heap over one cursor per run.
 */
class PhysicalExternalMergeSort : public PhysicalOperator {
 public:
  PhysicalExternalMergeSort(SchemaRef output_schema, std::vector<OrderBy> order_bys,
                            std::unique_ptr<PhysicalOperator> child);

  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;

  auto IsSource() const -> bool override { return true; }
  auto IsOrderPreserving() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  auto ParamsToString() const -> std::string override { return "{ external }"; }

  std::vector<OrderBy> order_bys_;
  std::vector<OrderModifiers> modifiers_;
  SchemaRef run_schema_;  // [__key STRING] ++ input columns
};

}  // namespace bumblebee
