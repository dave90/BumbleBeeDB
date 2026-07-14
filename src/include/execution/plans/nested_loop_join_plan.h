//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// nested_loop_join_plan.h
//
// Identification: src/include/execution/plans/nested_loop_join_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "catalog/schema.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Joins two inputs by evaluating the predicate against every pair of tuples.
 *
 * This is the join the planner always emits; the optimizer rewrites it into a
 * HashJoinPlanNode when the predicate turns out to be a conjunction of equalities.
 */
class NestedLoopJoinPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a nested loop join.
   *
   * @param output_schema The output schema: the left schema followed by the right.
   * @param left The left (outer) child.
   * @param right The right (inner) child.
   * @param predicate The join predicate. A cross product's predicate is the constant `true`.
   * @param join_type INNER or LEFT.
   */
  NestedLoopJoinPlanNode(SchemaRef output_schema, AbstractPlanNodeRef left, AbstractPlanNodeRef right,
                         AbstractExpressionRef predicate, JoinType join_type)
      : AbstractPlanNode(std::move(output_schema), {std::move(left), std::move(right)}),
        predicate_(std::move(predicate)),
        join_type_(join_type) {}

  auto GetType() const -> PlanType override { return PlanType::NestedLoopJoin; }

  /** @return The join predicate. */
  auto Predicate() const -> const AbstractExpressionRef & { return predicate_; }

  /** @return The join type. */
  auto GetJoinType() const -> JoinType { return join_type_; }

  /** @return The left (outer) child. */
  auto GetLeftPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 2, "Nested loop joins should have exactly two children.");
    return GetChildAt(0);
  }

  /** @return The right (inner) child. */
  auto GetRightPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 2, "Nested loop joins should have exactly two children.");
    return GetChildAt(1);
  }

  /**
   * @brief The schema of a join: the left schema's columns followed by the right's.
   *
   * @param left The left child.
   * @param right The right child.
   * @return Schema The joined schema.
   */
  static auto InferJoinSchema(const AbstractPlanNode &left, const AbstractPlanNode &right) -> Schema;

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(NestedLoopJoinPlanNode);

  /** The join predicate. */
  AbstractExpressionRef predicate_;
  /** The join type. */
  JoinType join_type_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
