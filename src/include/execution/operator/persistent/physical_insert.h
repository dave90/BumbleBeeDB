//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_insert.h
//
// Identification: src/include/execution/operator/persistent/physical_insert.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>

#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A parallel MVCC insert sink that emits the affected-row count as its source role.
 *
 * As a **sink** it routes each input chunk through `MvccInsert` (write set + undo logs, first-committer-
 * wins). As a **source** it emits one row — the total count — read from its own sink state, so the count
 * flows through a `ResultCollector` like any query result. `MvccInsert` is per-page-latched, so N tasks
 * writing disjoint input morsels into one heap and one shared transaction is safe.
 */
class PhysicalInsert : public PhysicalOperator {
 public:
  PhysicalInsert(SchemaRef output_schema, table_oid_t table_oid, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::INSERT, std::move(output_schema), 1), table_oid_(table_oid) {
    children_.push_back(std::move(child));
  }

  // ---- sink role (consume the rows to insert) --------------------------------
  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;

  // ---- source role (emit the affected-row count) -----------------------------
  auto IsSource() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  auto ParamsToString() const -> std::string override;

  table_oid_t table_oid_;
};

}  // namespace bumblebee
