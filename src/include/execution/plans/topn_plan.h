//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// topn_plan.h
//
// Identification: src/include/execution/plans/topn_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_order_by.h"
#include "common/macros.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Emits the `n_` smallest tuples of its child by the sort keys.
 *
 * This is what a Sort directly under a Limit collapses into: it only has to keep
 * `n` tuples in flight rather than materializing and ordering the whole input.
 */
class TopNPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a top-N.
   *
   * @param output The output schema, which is the child's schema.
   * @param child The child plan.
   * @param order_bys The sort keys, most significant first.
   * @param n How many tuples to emit.
   */
  TopNPlanNode(SchemaRef output, AbstractPlanNodeRef child, std::vector<OrderBy> order_bys, std::size_t n)
      : AbstractPlanNode(std::move(output), {std::move(child)}), order_bys_(std::move(order_bys)), n_{n} {}

  auto GetType() const -> PlanType override { return PlanType::TopN; }

  /** @return How many tuples to emit. */
  auto GetN() const -> std::size_t { return n_; }

  /** @return The sort keys, most significant first. */
  auto GetOrderBy() const -> const std::vector<OrderBy> & { return order_bys_; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "TopN should have exactly one child plan.");
    return GetChildAt(0);
  }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(TopNPlanNode);

  /** The sort keys, most significant first. */
  std::vector<OrderBy> order_bys_;
  /** How many tuples to emit. */
  std::size_t n_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
