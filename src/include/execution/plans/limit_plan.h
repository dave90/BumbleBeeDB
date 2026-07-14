//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// limit_plan.h
//
// Identification: src/include/execution/plans/limit_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Emits at most `limit_` tuples of its child.
 */
class LimitPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a limit.
   *
   * @param output The output schema, which is the child's schema.
   * @param child The child plan.
   * @param limit The maximum number of tuples to emit.
   */
  LimitPlanNode(SchemaRef output, AbstractPlanNodeRef child, std::size_t limit)
      : AbstractPlanNode(std::move(output), {std::move(child)}), limit_{limit} {}

  auto GetType() const -> PlanType override { return PlanType::Limit; }

  /** @return The maximum number of tuples to emit. */
  auto GetLimit() const -> std::size_t { return limit_; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Limit should have exactly one child plan.");
    return GetChildAt(0);
  }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(LimitPlanNode);

  /** The maximum number of tuples to emit. */
  std::size_t limit_;

 protected:
  auto PlanNodeToString() const -> std::string override { return fmt::format("Limit {{ limit={} }}", limit_); }
};

}  // namespace bumblebee
