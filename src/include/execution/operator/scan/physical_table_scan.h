//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_table_scan.h
//
// Identification: src/include/execution/operator/scan/physical_table_scan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "concurrency/transaction.h"  // ScanPredicate
#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A morsel-parallel scan of a row-format `TableHeap`, reading the MVCC-visible version per row.
 *
 * The row→vector gather, deleted-slot skipping and MVCC visibility all live inside the storage layer's
 * `HeapScan`; this operator just drives a cursor. Its `GlobalSourceState` holds the shared
 * `ParallelScanState` (page-directory snapshot + Halloween boundary + read set, taken once at open);
 * each `LocalSourceState` owns a per-morsel cursor.
 */
class PhysicalTableScan : public PhysicalOperator {
 public:
  PhysicalTableScan(SchemaRef output_schema, table_oid_t table_oid, std::string table_name, idx_t estimated_cardinality,
                    ScanPredicate predicate = {}, std::vector<idx_t> projection = {}, bool emit_rids = false)
      : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(output_schema), estimated_cardinality),
        table_oid_(table_oid),
        table_name_(std::move(table_name)),
        predicate_(std::move(predicate)),
        projection_(std::move(projection)),
        emit_rids_(emit_rids) {}

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
  ScanPredicate predicate_;
  std::vector<idx_t> projection_;
  /** When true, the scan appends each row's RID as a trailing BIGINT column (for Update/Delete). */
  bool emit_rids_;
};

}  // namespace bumblebee
