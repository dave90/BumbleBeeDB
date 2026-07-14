//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// filter_plan.h
//
// Identification: src/include/execution/plans/filter_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Keeps only the tuples of its child for which the predicate holds.
 */
class FilterPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a filter.
   *
   * @param output The output schema, which is the child's schema.
   * @param predicate The predicate to keep tuples by.
   * @param child The child plan.
   */
  FilterPlanNode(SchemaRef output, AbstractExpressionRef predicate, AbstractPlanNodeRef child)
      : AbstractPlanNode(std::move(output), {std::move(child)}), predicate_{std::move(predicate)} {}

  auto GetType() const -> PlanType override { return PlanType::Filter; }

  /** @return The predicate. */
  auto GetPredicate() const -> const AbstractExpressionRef & { return predicate_; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Filter should have exactly one child plan.");
    return GetChildAt(0);
  }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(FilterPlanNode);

  /** The predicate. */
  AbstractExpressionRef predicate_;

 protected:
  auto PlanNodeToString() const -> std::string override {
    return fmt::format("Filter {{ predicate={} }}", predicate_);
  }
};

}  // namespace bumblebee
