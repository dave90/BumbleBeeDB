//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// sort_plan.h
//
// Identification: src/include/execution/plans/sort_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_order_by.h"
#include "common/macros.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Sorts all the tuples of its child.
 */
class SortPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a sort.
   *
   * @param output The output schema, which is the child's schema.
   * @param child The child plan.
   * @param order_bys The sort keys, most significant first.
   */
  SortPlanNode(SchemaRef output, AbstractPlanNodeRef child, std::vector<OrderBy> order_bys)
      : AbstractPlanNode(std::move(output), {std::move(child)}), order_bys_(std::move(order_bys)) {}

  auto GetType() const -> PlanType override { return PlanType::Sort; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Sort should have exactly one child plan.");
    return GetChildAt(0);
  }

  /** @return The sort keys, most significant first. */
  auto GetOrderBy() const -> const std::vector<OrderBy> & { return order_bys_; }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(SortPlanNode);

  /** The sort keys, most significant first. */
  std::vector<OrderBy> order_bys_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
