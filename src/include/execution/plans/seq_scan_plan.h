//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// seq_scan_plan.h
//
// Identification: src/include/execution/plans/seq_scan_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/table_ref/bound_base_table_ref.h"
#include "fmt/ranges.h"
#include "catalog/schema.h"
#include "common/config.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Scans a table end to end. A leaf.
 */
class SeqScanPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a sequential scan.
   *
   * @param output The output schema.
   * @param table_oid The table to scan.
   * @param table_name The table's name, carried for readable plan output.
   * @param filter_predicate A predicate pushed down into the scan, or nullptr. The
   *        MergeFilterScan rule puts it here so the scan can skip tuples without a
   *        separate Filter node above it.
   */
  SeqScanPlanNode(SchemaRef output, table_oid_t table_oid, std::string table_name,
                  AbstractExpressionRef filter_predicate = nullptr)
      : AbstractPlanNode(std::move(output), {}),
        table_oid_{table_oid},
        table_name_(std::move(table_name)),
        filter_predicate_(std::move(filter_predicate)) {}

  auto GetType() const -> PlanType override { return PlanType::SeqScan; }

  /** @return The table being scanned. */
  auto GetTableOid() const -> table_oid_t { return table_oid_; }

  /**
   * @brief The schema a scan of `table_ref` produces.
   *
   * The columns are renamed to `table.column` so that a self-join's two sides stay
   * distinguishable in the plan output.
   *
   * @param table_ref The bound table reference.
   * @return Schema The scan's output schema.
   */
  static auto InferScanSchema(const BoundBaseTableRef &table_ref) -> Schema;

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(SeqScanPlanNode);

  /** The table being scanned. */
  table_oid_t table_oid_;

  /** The table's name. */
  std::string table_name_;

  /** A predicate pushed down into the scan, or nullptr. */
  AbstractExpressionRef filter_predicate_;

  /** Column indices some operator above actually reads (set by OptimizeColumnPruning; empty =
   * all). The scan's output schema stays full-width — unlisted columns are simply never
   * materialized and surface as constant-NULL vectors nothing reads. */
  std::vector<idx_t> pruned_columns_;

 protected:
  auto PlanNodeToString() const -> std::string override {
    if (!pruned_columns_.empty()) {
      return filter_predicate_ != nullptr
                 ? fmt::format("SeqScan {{ table={}, filter={}, columns=[{}] }}", table_name_, filter_predicate_,
                               fmt::join(pruned_columns_, ", "))
                 : fmt::format("SeqScan {{ table={}, columns=[{}] }}", table_name_, fmt::join(pruned_columns_, ", "));
    }
    if (filter_predicate_ != nullptr) {
      return fmt::format("SeqScan {{ table={}, filter={} }}", table_name_, filter_predicate_);
    }
    return fmt::format("SeqScan {{ table={} }}", table_name_);
  }
};

}  // namespace bumblebee
