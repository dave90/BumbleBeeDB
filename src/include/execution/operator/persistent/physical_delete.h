//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_delete.h
//
// Identification: src/include/execution/operator/persistent/physical_delete.h
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
 * @brief A parallel MVCC delete sink: reads the RID column its child scan emits and tombstones each row.
 *
 * Like `PhysicalInsert`, it is sink+source — the source role emits the affected-row count. Its child is
 * a scan (with the WHERE folded in) whose last output column is the RID.
 */
class PhysicalDelete : public PhysicalOperator {
 public:
  PhysicalDelete(SchemaRef output_schema, table_oid_t table_oid, std::unique_ptr<PhysicalOperator> child,
                 idx_t rid_column)
      : PhysicalOperator(PhysicalOperatorType::DELETE, std::move(output_schema), 1),
        table_oid_(table_oid),
        rid_column_(rid_column) {
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  // Buffers RIDs; the actual MvccDelete runs in Combine, AFTER the scan releases its page guards
  // (deleting a row whose page the scan still read-latches would self-deadlock).
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;

  auto IsSource() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  table_oid_t table_oid_;
  idx_t rid_column_;  // index of the RID column in the child's output
};

}  // namespace bumblebee
