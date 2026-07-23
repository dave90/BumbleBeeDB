//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column_pruning.cpp
//
// Identification: src/optimizer/column_pruning.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <set>

#include "execution/expressions/column_value_expression.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/topn_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

namespace {

/** @brief Collect the column indices `expr` reads from input tuple `tuple_idx`. */
void CollectColumnRefs(const AbstractExpressionRef &expr, uint32_t tuple_idx, std::set<idx_t> &out) {
  if (expr == nullptr) {
    return;
  }
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get());
      col != nullptr && col->GetTupleIdx() == tuple_idx) {
    out.insert(col->GetColIdx());
  }
  for (const auto &child : expr->GetChildren()) {
    CollectColumnRefs(child, tuple_idx, out);
  }
}

auto AllColumnsOf(const AbstractPlanNode &node) -> std::set<idx_t> {
  std::set<idx_t> all;
  for (idx_t i = 0; i < node.output_schema_->GetColumnCount(); i++) {
    all.insert(i);
  }
  return all;
}

/**
 * @brief Rebuild `plan` bottom-up, telling every SeqScan which of its columns some ancestor
 * actually reads (`required` = the referenced indices of THIS node's output).
 *
 * Schemas and column numbering are deliberately left untouched everywhere — the trap in pruning
 * is renumbering references above a narrowed node. Instead the scan keeps its full-width output
 * shape and simply never materializes the unreferenced columns (they surface as constant-NULL
 * vectors that, by construction of `required`, no operator ever reads).
 */
auto PruneColumns(const AbstractPlanNodeRef &plan, std::set<idx_t> required) -> AbstractPlanNodeRef {
  switch (plan->GetType()) {
    case PlanType::SeqScan: {
      const auto &scan = dynamic_cast<const SeqScanPlanNode &>(*plan);
      // The scan itself reads its pushed-down predicate's columns.
      CollectColumnRefs(scan.filter_predicate_, 0, required);
      const auto total = scan.output_schema_->GetColumnCount();
      if (required.size() >= total) {
        return plan;  // everything is read: nothing to prune
      }
      // The storage backends cannot express a zero-column scan (an empty projection means "all"
      // to the heap), so COUNT(*)-style plans keep one column alive.
      if (required.empty()) {
        required.insert(0);
      }
      auto pruned = std::make_shared<SeqScanPlanNode>(scan.output_schema_, scan.table_oid_, scan.table_name_,
                                                      scan.filter_predicate_);
      pruned->pruned_columns_.assign(required.begin(), required.end());
      return pruned;
    }
    case PlanType::Projection: {
      const auto &proj = dynamic_cast<const ProjectionPlanNode &>(*plan);
      // Only the expressions some ancestor reads contribute their column references; an unused
      // projection output never observably runs, so its inputs may be pruned away.
      std::set<idx_t> child_req;
      const auto &exprs = proj.GetExpressions();
      for (idx_t i = 0; i < exprs.size(); i++) {
        if (required.contains(i)) {
          CollectColumnRefs(exprs[i], 0, child_req);
        }
      }
      auto child = PruneColumns(proj.GetChildPlan(), std::move(child_req));
      return plan->CloneWithChildren({std::move(child)});
    }
    case PlanType::Filter: {
      const auto &filter = dynamic_cast<const FilterPlanNode &>(*plan);
      CollectColumnRefs(filter.GetPredicate(), 0, required);
      auto child = PruneColumns(filter.GetChildPlan(), std::move(required));
      return plan->CloneWithChildren({std::move(child)});
    }
    case PlanType::Limit: {
      auto child = PruneColumns(plan->GetChildAt(0), std::move(required));
      return plan->CloneWithChildren({std::move(child)});
    }
    case PlanType::Sort: {
      const auto &sort = dynamic_cast<const SortPlanNode &>(*plan);
      for (const auto &order_by : sort.GetOrderBy()) {
        CollectColumnRefs(std::get<2>(order_by), 0, required);
      }
      auto child = PruneColumns(plan->GetChildAt(0), std::move(required));
      return plan->CloneWithChildren({std::move(child)});
    }
    case PlanType::TopN: {
      const auto &topn = dynamic_cast<const TopNPlanNode &>(*plan);
      for (const auto &order_by : topn.GetOrderBy()) {
        CollectColumnRefs(std::get<2>(order_by), 0, required);
      }
      auto child = PruneColumns(plan->GetChildAt(0), std::move(required));
      return plan->CloneWithChildren({std::move(child)});
    }
    case PlanType::Aggregation: {
      // The aggregation's output is keys + aggregates, not a subset of its child's columns; its
      // child needs exactly what the group-bys and aggregate arguments read.
      const auto &agg = dynamic_cast<const AggregationPlanNode &>(*plan);
      std::set<idx_t> child_req;
      for (const auto &e : agg.GetGroupBys()) {
        CollectColumnRefs(e, 0, child_req);
      }
      for (const auto &e : agg.GetAggregates()) {
        CollectColumnRefs(e, 0, child_req);
      }
      auto child = PruneColumns(plan->GetChildAt(0), std::move(child_req));
      return plan->CloneWithChildren({std::move(child)});
    }
    case PlanType::HashJoin: {
      // Output = left columns ++ right columns: split the requirement at the seam and add each
      // side's join keys (key expressions are evaluated against their own side, tuple 0).
      const auto &join = dynamic_cast<const HashJoinPlanNode &>(*plan);
      const auto left_count = join.GetLeftPlan()->output_schema_->GetColumnCount();
      std::set<idx_t> left_req;
      std::set<idx_t> right_req;
      for (auto i : required) {
        if (i < left_count) {
          left_req.insert(i);
        } else {
          right_req.insert(i - left_count);
        }
      }
      // Key expressions are evaluated against their own side's chunk, but by convention the left
      // keys are written as tuple 0 and the right keys as tuple 1; collect both spaces so either
      // convention only ever over-collects.
      for (const auto &k : join.LeftJoinKeyExpressions()) {
        CollectColumnRefs(k, 0, left_req);
        CollectColumnRefs(k, 1, left_req);
      }
      for (const auto &k : join.RightJoinKeyExpressions()) {
        CollectColumnRefs(k, 0, right_req);
        CollectColumnRefs(k, 1, right_req);
      }
      auto left = PruneColumns(join.GetLeftPlan(), std::move(left_req));
      auto right = PruneColumns(join.GetRightPlan(), std::move(right_req));
      return plan->CloneWithChildren({std::move(left), std::move(right)});
    }
    case PlanType::NestedLoopJoin: {
      // Same seam split; the predicate references the left as tuple 0 and the right as tuple 1.
      const auto &join = dynamic_cast<const NestedLoopJoinPlanNode &>(*plan);
      const auto left_count = join.GetLeftPlan()->output_schema_->GetColumnCount();
      std::set<idx_t> left_req;
      std::set<idx_t> right_req;
      for (auto i : required) {
        if (i < left_count) {
          left_req.insert(i);
        } else {
          right_req.insert(i - left_count);
        }
      }
      CollectColumnRefs(join.Predicate(), 0, left_req);
      CollectColumnRefs(join.Predicate(), 1, right_req);
      auto left = PruneColumns(join.GetLeftPlan(), std::move(left_req));
      auto right = PruneColumns(join.GetRightPlan(), std::move(right_req));
      return plan->CloneWithChildren({std::move(left), std::move(right)});
    }
    default: {
      // Insert / Update / Delete / Values / anything new: conservatively require every column of
      // every child (DML sinks really do read whole rows).
      std::vector<AbstractPlanNodeRef> children;
      children.reserve(plan->GetChildren().size());
      for (const auto &child : plan->GetChildren()) {
        children.push_back(PruneColumns(child, AllColumnsOf(*child)));
      }
      return plan->CloneWithChildren(std::move(children));
    }
  }
}

}  // namespace

/**
 * @brief Projection pushdown into the scans: a column no operator above ever reads is never
 * materialized (heap: no row->vector gather; parquet: the column's pages are never even decoded).
 *
 * Runs LAST in the optimizer, after predicates have been merged into the scans (their columns
 * count as read) and joins have taken their final shape.
 */
auto Optimizer::OptimizeColumnPruning(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  return PruneColumns(plan, AllColumnsOf(*plan));
}

}  // namespace bumblebee
