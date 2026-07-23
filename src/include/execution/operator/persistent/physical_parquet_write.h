//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_parquet_write.h
//
// Identification: src/include/execution/operator/persistent/physical_parquet_write.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief INSERT into an external parquet table: append-only, non-transactional.
 *
 * Sink tasks buffer their input; Finalize — the commit point — takes the fail-fast writer lock,
 * writes ONE new part file and swaps in manifest N+1 (= N's files + the new one). No MVCC, no
 * undo: the surrounding bookkeeping transaction's commit/abort has no effect on the files.
 * Sink+source — the source emits the inserted-row count.
 */
class PhysicalParquetInsert : public PhysicalOperator {
 public:
  PhysicalParquetInsert(SchemaRef output_schema, table_oid_t table_oid, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::PARQUET_INSERT, std::move(output_schema), 1), table_oid_(table_oid) {
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
  auto ParamsToString() const -> std::string override;

  table_oid_t table_oid_;
};

/**
 * @brief DELETE from an external parquet table: copy-on-write, non-transactional.
 *
 * Sink tasks collect doomed RIDs (from the scan's trailing RID column). Finalize takes the writer
 * lock, rewrites ONLY the files containing doomed rows (survivor rows stream into fresh part
 * files; untouched files carry forward), swaps the manifest, and unlinks the replaced files.
 * Deleting every row is legal: the manifest's file list just becomes empty.
 */
class PhysicalParquetDelete : public PhysicalOperator {
 public:
  PhysicalParquetDelete(SchemaRef output_schema, table_oid_t table_oid, std::unique_ptr<PhysicalOperator> child,
                        idx_t rid_column)
      : PhysicalOperator(PhysicalOperatorType::PARQUET_DELETE, std::move(output_schema), 1),
        table_oid_(table_oid),
        rid_column_(rid_column) {
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
  auto ParamsToString() const -> std::string override;

  table_oid_t table_oid_;
  idx_t rid_column_;  // index of the RID column in the child's output
};

/**
 * @brief UPDATE of an external parquet table: copy-on-write, non-transactional.
 *
 * Sink tasks evaluate the target expressions per matched row and keep (RID -> new row). Finalize
 * takes the writer lock and streams the files containing matched rows, substituting the new rows
 * in place, then swaps the manifest (same file-granular rewrite as DELETE).
 */
class PhysicalParquetUpdate : public PhysicalOperator {
 public:
  PhysicalParquetUpdate(SchemaRef output_schema, table_oid_t table_oid, std::unique_ptr<PhysicalOperator> child,
                        idx_t rid_column, std::vector<AbstractExpressionRef> target_expressions)
      : PhysicalOperator(PhysicalOperatorType::PARQUET_UPDATE, std::move(output_schema), 1),
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
  auto ParamsToString() const -> std::string override;

  table_oid_t table_oid_;
  idx_t rid_column_;
  std::vector<AbstractExpressionRef> target_expressions_;
  std::vector<LogicalType> new_types_;
};

}  // namespace bumblebee
