//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// sort_limit_as_topn.cpp
//
// Identification: src/optimizer/sort_limit_as_topn.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <vector>

#include "common/macros.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/topn_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

/**
 * @brief Collapse a Limit directly over a Sort into a single TopN.
 *
 * `ORDER BY x LIMIT 10` is planned as a Sort under a Limit, which sorts the entire
 * input — O(n log n) time and O(n) memory — just to keep ten rows. A TopN keeps a
 * bounded heap of ten instead, so it runs in O(n log k) time and O(k) memory.
 *
 * The Sort must be the Limit's immediate child. `ORDER BY` under anything else, or
 * a Sort with no Limit above it, has to stay a full sort.
 */
auto Optimizer::OptimizeSortLimitAsTopN(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeSortLimitAsTopN(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::Limit) {
    return optimized_plan;
  }

  const auto &limit_plan = dynamic_cast<const LimitPlanNode &>(*optimized_plan);
  BUMBLEBEE_ENSURE(optimized_plan->children_.size() == 1, "Limit should have exactly one child.");

  const auto &child_plan = optimized_plan->children_[0];
  if (child_plan->GetType() != PlanType::Sort) {
    return optimized_plan;
  }

  const auto &sort_plan = dynamic_cast<const SortPlanNode &>(*child_plan);
  BUMBLEBEE_ENSURE(child_plan->GetChildren().size() == 1, "Sort should have exactly one child.");

  // The TopN takes the Sort's child directly: both the Limit and the Sort disappear.
  return std::make_shared<TopNPlanNode>(limit_plan.output_schema_, sort_plan.GetChildPlan(),
                                        sort_plan.GetOrderBy(), limit_plan.GetLimit());
}

}  // namespace bumblebee
