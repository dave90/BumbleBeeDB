//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// merge_filter_nlj.cpp
//
// Identification: src/optimizer/merge_filter_nlj.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <vector>

#include "common/exception.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

auto Optimizer::RewriteExpressionForJoin(const AbstractExpressionRef &expr, size_t left_column_cnt,
                                         size_t right_column_cnt) -> AbstractExpressionRef {
  std::vector<AbstractExpressionRef> children;
  children.reserve(expr->GetChildren().size());
  for (const auto &child : expr->GetChildren()) {
    children.emplace_back(RewriteExpressionForJoin(child, left_column_cnt, right_column_cnt));
  }

  if (const auto *column_value_expr = dynamic_cast<const ColumnValueExpression *>(expr.get());
      column_value_expr != nullptr) {
    // Before this rule runs, the predicate came off a Filter sitting above a cross
    // product, so every reference is against one flat schema and must be tuple 0.
    BUMBLEBEE_ENSURE(column_value_expr->GetTupleIdx() == 0,
                     "tuple_idx must be 0 before the join predicate is re-indexed");
    auto col_idx = column_value_expr->GetColIdx();
    if (col_idx < left_column_cnt) {
      return std::make_shared<ColumnValueExpression>(0, col_idx, column_value_expr->GetReturnType());
    }
    if (col_idx < left_column_cnt + right_column_cnt) {
      return std::make_shared<ColumnValueExpression>(1, col_idx - left_column_cnt,
                                                     column_value_expr->GetReturnType());
    }
    throw Exception(fmt::format("column index {} is past the end of the join's inputs", col_idx));
  }

  return expr->CloneWithChildren(children);
}

auto Optimizer::IsPredicateTrue(const AbstractExpressionRef &expr) -> bool {
  if (expr == nullptr) {
    return false;
  }
  if (const auto *const_expr = dynamic_cast<const ConstantValueExpression *>(expr.get());
      const_expr != nullptr) {
    if (const_expr->val_.IsNull()) {
      return false;
    }
    return const_expr->val_.CastAs(LogicalTypeId::BOOLEAN).GetAs<int8_t>() != 0;
  }
  return false;
}

/**
 * @brief Fold a Filter sitting on a cross-product join into the join's predicate.
 *
 * The planner always emits `SELECT * FROM a, b WHERE a.x = b.y` as a cross product
 * with an always-true predicate, plus a Filter above it. Executing that literally
 * would materialize the full cartesian product and then throw most of it away, so
 * we move the filter's predicate into the join — where it can actually restrict
 * which pairs are produced — and re-index its column references from the flat
 * `#0.k` addressing the filter used to the two-sided `#0.k` / `#1.k` a join needs.
 *
 * Only fires when the join's own predicate is still the constant `true`; if the
 * join already carries a predicate, we would be silently dropping it.
 */
auto Optimizer::OptimizeMergeFilterNLJ(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeMergeFilterNLJ(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::Filter) {
    return optimized_plan;
  }

  const auto &filter_plan = dynamic_cast<const FilterPlanNode &>(*optimized_plan);
  BUMBLEBEE_ENSURE(optimized_plan->children_.size() == 1, "Filter should have exactly one child.");

  const auto &child_plan = optimized_plan->children_[0];
  if (child_plan->GetType() != PlanType::NestedLoopJoin) {
    return optimized_plan;
  }

  const auto &nlj_plan = dynamic_cast<const NestedLoopJoinPlanNode &>(*child_plan);
  BUMBLEBEE_ENSURE(child_plan->GetChildren().size() == 2, "NestedLoopJoin should have exactly two children.");

  if (!IsPredicateTrue(nlj_plan.Predicate())) {
    return optimized_plan;
  }

  return std::make_shared<NestedLoopJoinPlanNode>(
      filter_plan.output_schema_, nlj_plan.GetLeftPlan(), nlj_plan.GetRightPlan(),
      RewriteExpressionForJoin(filter_plan.GetPredicate(),
                               nlj_plan.GetLeftPlan()->OutputSchema().GetColumnCount(),
                               nlj_plan.GetRightPlan()->OutputSchema().GetColumnCount()),
      nlj_plan.GetJoinType());
}

}  // namespace bumblebee
