//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// merge_projection.cpp
//
// Identification: src/optimizer/merge_projection.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <memory>
#include <vector>

#include "catalog/schema.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/projection_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

/**
 * @brief Collapse a projection that just re-emits its child's columns unchanged.
 *
 * The planner produces identity projections routinely — from `SELECT *`, from
 * aggregation, and whenever it needs to rename columns — and each one costs a full
 * pass over the data at execution time for no benefit. If every expression is a
 * plain reference to the correspondingly-positioned child column, and the types
 * line up, we drop the projection and keep only its schema (which carries the
 * output column names).
 */
auto Optimizer::OptimizeMergeProjection(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeMergeProjection(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::Projection) {
    return optimized_plan;
  }

  const auto &projection_plan = dynamic_cast<const ProjectionPlanNode &>(*optimized_plan);
  BUMBLEBEE_ENSURE(optimized_plan->children_.size() == 1, "Projection should have exactly one child.");

  const auto &child_plan = optimized_plan->children_[0];
  const auto &child_columns = child_plan->OutputSchema().GetColumns();
  const auto &projection_schema = projection_plan.OutputSchema();
  const auto &projection_columns = projection_schema.GetColumns();

  // The column names may differ — that is exactly what the projection is for — but
  // the types must line up one for one.
  if (!std::equal(child_columns.begin(), child_columns.end(), projection_columns.begin(),
                  projection_columns.end(),
                  [](auto &&child_col, auto &&proj_col) { return child_col.GetType() == proj_col.GetType(); })) {
    return optimized_plan;
  }

  // Every expression must be a bare reference to the child column in the same position.
  const auto &exprs = projection_plan.GetExpressions();
  for (size_t idx = 0; idx < exprs.size(); idx++) {
    const auto *column_value_expr = dynamic_cast<const ColumnValueExpression *>(exprs[idx].get());
    if (column_value_expr == nullptr || column_value_expr->GetTupleIdx() != 0 ||
        column_value_expr->GetColIdx() != idx) {
      return optimized_plan;
    }
  }

  // Keep the projection's schema — it holds the output column names — but drop the
  // projection node itself.
  auto merged = child_plan->CloneWithChildren(child_plan->GetChildren());
  merged->output_schema_ = std::make_shared<Schema>(projection_schema);
  return merged;
}

}  // namespace bumblebee
