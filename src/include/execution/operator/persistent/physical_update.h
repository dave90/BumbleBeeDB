//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_update.h
//
// Identification: src/include/execution/operator/persistent/physical_update.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "execution/physical_operator.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * @brief A parallel MVCC update sink: recompute each targeted row's columns and version the pre-image.
 *
 * Its child scan emits the old table columns plus a trailing RID column. Per input chunk it evaluates
 * the target expressions (one per table column; unchanged columns re-reference the old value) into a new
 * chunk and calls `MvccUpdate`. Sink+source — the source emits the affected-row count.
 *
 * Note: the landed `MvccUpdate` requires a same-byte-size replacement, so updates that change a VARCHAR
 * length are out of scope for now; fixed-width columns are fine.
 */
class PhysicalUpdate : public PhysicalOperator {
 public:
  PhysicalUpdate(SchemaRef output_schema, table_oid_t table_oid, std::unique_ptr<PhysicalOperator> child,
                 idx_t rid_column, std::vector<AbstractExpressionRef> target_expressions)
      : PhysicalOperator(PhysicalOperatorType::UPDATE, std::move(output_schema), 1),
        table_oid_(table_oid),
        rid_column_(rid_column),
        target_expressions_(std::move(target_expressions)) {
    for (const auto &e : target_expressions_) {
      new_types_.push_back(e->GetReturnType().GetType());
    }
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  // Buffers RIDs + pre-image/replacement rows; MvccUpdate runs in Combine (after the scan releases page
  // guards), and index maintenance is deferred to Finalize so it sees the statement's whole set of key
  // moves at once (a permutation split across chunks / parallel workers must not false-conflict).
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

  table_oid_t table_oid_;
  idx_t rid_column_;
  std::vector<AbstractExpressionRef> target_expressions_;
  std::vector<LogicalType> new_types_;
};

}  // namespace bumblebee
