//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_parquet_scan.h
//
// Identification: src/include/execution/operator/scan/physical_parquet_scan.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A morsel-parallel scan of an external parquet table.
 *
 * The `GlobalSourceState` reads the newest manifest once at open (the statement-level snapshot)
 * and flattens the live files' row groups into the morsel space: one morsel = one (file, row
 * group) pair, claimed lock-free. Each worker decodes its row group chunk-by-chunk through a
 * `ParquetReader`. No MVCC is involved — manifest visibility is the snapshot.
 *
 * With `emit_rids_`, the trailing output column receives each row's synthetic identifier
 * `(file_index << 32) | row_index_in_file`, which the external write operators resolve back
 * against the same snapshot.
 */
class PhysicalParquetScan : public PhysicalOperator {
 public:
  PhysicalParquetScan(SchemaRef output_schema, table_oid_t table_oid, std::string table_name,
                      idx_t estimated_cardinality, bool emit_rids = false, AbstractExpressionRef predicate = nullptr,
                      std::vector<idx_t> projection = {})
      : PhysicalOperator(PhysicalOperatorType::PARQUET_SCAN, std::move(output_schema), estimated_cardinality),
        table_oid_(table_oid),
        table_name_(std::move(table_name)),
        emit_rids_(emit_rids),
        predicate_(std::move(predicate)),
        projection_(std::move(projection)) {}

  auto IsSource() const -> bool override { return true; }

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const
      -> std::unique_ptr<LocalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType override;

  auto ParamsToString() const -> std::string override;

  table_oid_t table_oid_;
  std::string table_name_;
  /** When true, the scan appends each row's synthetic RID as a trailing BIGINT column. */
  bool emit_rids_;
  /** The scan's WHERE (still fully applied by the filter above); used only for row-group
   * pruning against parquet min/max statistics. */
  AbstractExpressionRef predicate_;
  /** Columns some operator above reads (empty = all): only these are decoded; the rest of the
   * full-width output surfaces as constant-NULL vectors nothing reads. */
  std::vector<idx_t> projection_;
};

}  // namespace bumblebee
