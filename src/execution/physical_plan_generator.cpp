//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_plan_generator.cpp
//
// Identification: src/execution/physical_plan_generator.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/physical_plan_generator.h"

#include <memory>
#include <utility>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "execution/operator/aggregate/physical_hash_aggregate.h"
#include "execution/operator/aggregate/physical_ungrouped_aggregate.h"
#include "execution/operator/filter/physical_filter.h"
#include "execution/operator/helper/physical_result_collector.h"
#include "execution/operator/join/physical_grace_hash_join.h"
#include "execution/operator/join/physical_hash_join.h"
#include "execution/operator/join/physical_nested_loop_join.h"
#include "execution/operator/persistent/physical_delete.h"
#include "execution/operator/persistent/physical_insert.h"
#include "execution/operator/order/physical_external_merge_sort.h"
#include "execution/operator/order/physical_limit.h"
#include "execution/operator/order/physical_sort.h"
#include "execution/operator/order/physical_top_n.h"
#include "execution/operator/persistent/physical_update.h"
#include "execution/operator/projection/physical_projection.h"
#include "execution/operator/scan/physical_table_scan.h"
#include "execution/operator/scan/physical_values.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/delete_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/insert_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/topn_plan.h"
#include "execution/plans/update_plan.h"
#include "execution/plans/values_plan.h"

namespace bumblebee {

auto PhysicalPlanGenerator::CreatePlan(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  switch (plan->GetType()) {
    case PlanType::SeqScan:
      return CreateSeqScan(plan);
    case PlanType::Filter:
      return CreateFilter(plan);
    case PlanType::Projection:
      return CreateProjection(plan);
    case PlanType::Values:
      return CreateValues(plan);
    case PlanType::Insert:
      return CreateInsert(plan);
    case PlanType::Delete:
      return CreateDelete(plan);
    case PlanType::Update:
      return CreateUpdate(plan);
    case PlanType::Sort:
      return CreateSort(plan);
    case PlanType::Limit:
      return CreateLimit(plan);
    case PlanType::TopN:
      return CreateTopN(plan);
    case PlanType::Aggregation:
      return CreateAggregation(plan);
    case PlanType::HashJoin:
      return CreateHashJoin(plan);
    case PlanType::NestedLoopJoin:
      return CreateNestedLoopJoin(plan);
    default:
      throw NotImplementedException("physical lowering not implemented for this plan node yet");
  }
}

namespace {

/** @brief `base` with a trailing BIGINT RID column appended (the scan-emitted row identifier). */
auto AppendRidColumn(const SchemaRef &base) -> SchemaRef {
  std::vector<Column> cols = base->GetColumns();
  cols.emplace_back("__rid", LogicalType(LogicalTypeId::BIGINT));
  return std::make_shared<const Schema>(cols);
}

}  // namespace

auto PhysicalPlanGenerator::LowerDmlChild(const AbstractPlanNodeRef &child, idx_t &rid_column)
    -> std::unique_ptr<PhysicalOperator> {
  // The rows to update/delete come from a scan (WHERE folded in) or a filter over one. The scan emits a
  // trailing RID column so the write sink knows which rows to modify; a filter above it slices it through.
  if (child->GetType() == PlanType::SeqScan) {
    const auto &scan = dynamic_cast<const SeqScanPlanNode &>(*child);
    auto rid_schema = AppendRidColumn(child->output_schema_);
    rid_column = rid_schema->GetColumnCount() - 1;
    std::unique_ptr<PhysicalOperator> op =
        std::make_unique<PhysicalTableScan>(rid_schema, scan.GetTableOid(), scan.table_name_,
                                            rid_schema->GetColumnCount(), ScanPredicate{}, std::vector<idx_t>{},
                                            /*emit_rids=*/true);
    if (scan.filter_predicate_ != nullptr) {
      op = std::make_unique<PhysicalFilter>(rid_schema, scan.filter_predicate_, std::move(op));
    }
    return op;
  }
  if (child->GetType() == PlanType::Filter) {
    const auto &filter = dynamic_cast<const FilterPlanNode &>(*child);
    auto scan_op = LowerDmlChild(filter.GetChildPlan(), rid_column);
    auto rid_schema = scan_op->output_schema_;  // carries the RID column
    return std::make_unique<PhysicalFilter>(rid_schema, filter.GetPredicate(), std::move(scan_op));
  }
  throw NotImplementedException("UPDATE/DELETE source must be a table scan (optionally filtered)");
}

auto PhysicalPlanGenerator::PlanRoot(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  auto child = CreatePlan(plan);
  if (child->type_ == PhysicalOperatorType::RESULT_COLLECTOR) {
    return child;
  }
  // Everything else — SELECT trees and the sink+source DML operators — streams its output rows (or its
  // affected-row count) through a result collector.
  return std::make_unique<PhysicalResultCollector>(std::move(child));
}

auto PhysicalPlanGenerator::CreateSeqScan(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &scan = dynamic_cast<const SeqScanPlanNode &>(*plan);
  std::unique_ptr<PhysicalOperator> op = std::make_unique<PhysicalTableScan>(
      plan->output_schema_, scan.GetTableOid(), scan.table_name_, plan->output_schema_->GetColumnCount());
  // The optimizer folds a WHERE into the scan node. Until the row-at-a-time ScanPredicate compiler
  // lands, realize it as a streaming filter above the scan (correct, just not pushed into the gather).
  if (scan.filter_predicate_ != nullptr) {
    op = std::make_unique<PhysicalFilter>(plan->output_schema_, scan.filter_predicate_, std::move(op));
  }
  return op;
}

auto PhysicalPlanGenerator::CreateFilter(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &filter = dynamic_cast<const FilterPlanNode &>(*plan);
  auto child = CreatePlan(filter.GetChildPlan());
  return std::make_unique<PhysicalFilter>(plan->output_schema_, filter.GetPredicate(), std::move(child));
}

auto PhysicalPlanGenerator::CreateProjection(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &proj = dynamic_cast<const ProjectionPlanNode &>(*plan);
  auto child = CreatePlan(proj.GetChildPlan());
  return std::make_unique<PhysicalProjection>(plan->output_schema_, proj.GetExpressions(), std::move(child));
}

auto PhysicalPlanGenerator::CreateValues(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &values = dynamic_cast<const ValuesPlanNode &>(*plan);
  return std::make_unique<PhysicalValues>(plan->output_schema_, values.GetValues());
}

auto PhysicalPlanGenerator::CreateInsert(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &insert = dynamic_cast<const InsertPlanNode &>(*plan);
  auto child = CreatePlan(insert.GetChildPlan());
  return std::make_unique<PhysicalInsert>(plan->output_schema_, insert.GetTableOid(), std::move(child));
}

auto PhysicalPlanGenerator::CreateDelete(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &del = dynamic_cast<const DeletePlanNode &>(*plan);
  idx_t rid_column = 0;
  auto child = LowerDmlChild(del.GetChildPlan(), rid_column);
  return std::make_unique<PhysicalDelete>(plan->output_schema_, del.GetTableOid(), std::move(child), rid_column);
}

auto PhysicalPlanGenerator::CreateHashJoin(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &hj = dynamic_cast<const HashJoinPlanNode &>(*plan);
  auto left = CreatePlan(hj.GetLeftPlan());    // child 0 = left input
  auto right = CreatePlan(hj.GetRightPlan());  // child 1 = right input
  const idx_t left_cols = hj.GetLeftPlan()->output_schema_->GetColumnCount();
  std::unique_ptr<PhysicalOperator> op;
  // Both variants pick their build/probe side from the join type internally (the preserved side of a
  // LEFT join always streams as the probe), so INNER and LEFT may both lower — or be re-lowered on a
  // build overflow — to the external variant.
  if (UseExternal(plan)) {
    op = std::make_unique<PhysicalGraceHashJoin>(plan->output_schema_, hj.LeftJoinKeyExpressions(),
                                                 hj.RightJoinKeyExpressions(), hj.GetJoinType(), left_cols,
                                                 std::move(left), std::move(right));
  } else {
    op = std::make_unique<PhysicalHashJoin>(plan->output_schema_, hj.LeftJoinKeyExpressions(),
                                            hj.RightJoinKeyExpressions(), hj.GetJoinType(), left_cols,
                                            std::move(left), std::move(right));
  }
  op->logical_source_ = plan.get();  // so a build overflow can name this node for the retry
  return op;
}

auto PhysicalPlanGenerator::CreateNestedLoopJoin(const AbstractPlanNodeRef &plan)
    -> std::unique_ptr<PhysicalOperator> {
  const auto &nlj = dynamic_cast<const NestedLoopJoinPlanNode &>(*plan);
  auto outer = CreatePlan(nlj.GetLeftPlan());
  auto inner = CreatePlan(nlj.GetRightPlan());
  const idx_t left_cols = nlj.GetLeftPlan()->output_schema_->GetColumnCount();
  return std::make_unique<PhysicalNestedLoopJoin>(plan->output_schema_, nlj.Predicate(), nlj.GetJoinType(), left_cols,
                                                  std::move(outer), std::move(inner));
}

auto PhysicalPlanGenerator::CreateAggregation(const AbstractPlanNodeRef &plan)
    -> std::unique_ptr<PhysicalOperator> {
  const auto &agg = dynamic_cast<const AggregationPlanNode &>(*plan);
  auto child = CreatePlan(agg.GetChildPlan());
  if (agg.GetGroupBys().empty()) {
    return std::make_unique<PhysicalUngroupedAggregate>(plan->output_schema_, agg.GetAggregates(),
                                                        agg.GetAggregateTypes(), std::move(child));
  }
  return std::make_unique<PhysicalHashAggregate>(plan->output_schema_, agg.GetGroupBys(), agg.GetAggregates(),
                                                 agg.GetAggregateTypes(), std::move(child));
}

auto PhysicalPlanGenerator::CreateSort(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &sort = dynamic_cast<const SortPlanNode &>(*plan);
  auto child = CreatePlan(sort.GetChildPlan());
  std::unique_ptr<PhysicalOperator> op;
  if (UseExternal(plan)) {
    op = std::make_unique<PhysicalExternalMergeSort>(plan->output_schema_, sort.GetOrderBy(), std::move(child));
  } else {
    op = std::make_unique<PhysicalSort>(plan->output_schema_, sort.GetOrderBy(), std::move(child));
  }
  op->logical_source_ = plan.get();  // so an overflow can name this node for the retry
  return op;
}

auto PhysicalPlanGenerator::CreateLimit(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &limit = dynamic_cast<const LimitPlanNode &>(*plan);
  auto child = CreatePlan(limit.GetChildPlan());
  return std::make_unique<PhysicalLimit>(plan->output_schema_, limit.GetLimit(), std::move(child));
}

auto PhysicalPlanGenerator::CreateTopN(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &topn = dynamic_cast<const TopNPlanNode &>(*plan);
  auto child = CreatePlan(topn.GetChildPlan());
  return std::make_unique<PhysicalTopN>(plan->output_schema_, topn.GetOrderBy(), topn.GetN(), std::move(child));
}

auto PhysicalPlanGenerator::CreateUpdate(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator> {
  const auto &update = dynamic_cast<const UpdatePlanNode &>(*plan);
  idx_t rid_column = 0;
  auto child = LowerDmlChild(update.GetChildPlan(), rid_column);
  return std::make_unique<PhysicalUpdate>(plan->output_schema_, update.GetTableOid(), std::move(child), rid_column,
                                          update.target_expressions_);
}

}  // namespace bumblebee
