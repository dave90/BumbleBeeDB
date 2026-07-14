//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// eliminate_true_filter.cpp
//
// Identification: src/optimizer/eliminate_true_filter.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <vector>

#include "common/macros.h"
#include "execution/plans/filter_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

/**
 * @brief Drop a Filter whose predicate is the constant `true`.
 *
 * These come from cross products, whose predicate the planner sets to `true`, and
 * from `WHERE true`. Such a filter passes every tuple through, so it is pure
 * overhead.
 */
auto Optimizer::OptimizeEliminateTrueFilter(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeEliminateTrueFilter(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() == PlanType::Filter) {
    const auto &filter_plan = dynamic_cast<const FilterPlanNode &>(*optimized_plan);
    if (IsPredicateTrue(filter_plan.GetPredicate())) {
      BUMBLEBEE_ASSERT(optimized_plan->children_.size() == 1, "Filter should have exactly one child.");
      return optimized_plan->children_[0];
    }
  }

  return optimized_plan;
}

}  // namespace bumblebee
