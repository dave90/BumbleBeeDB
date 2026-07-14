//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// hash_join_plan.h
//
// Identification: src/include/execution/plans/hash_join_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "common/macros.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Joins two inputs by hashing the left keys and probing with the right.
 *
 * The two key lists are positional: `left_key_expressions_[i]` is matched against
 * `right_key_expressions_[i]`. The plan node says only that this is an equi-join
 * on those keys; how the hash table is built and probed is the engine's business
 * (BumbleBee's PRLHashTable, in a later milestone).
 */
class HashJoinPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a hash join.
   *
   * @param output_schema The output schema: the left schema followed by the right.
   * @param left The left (build) child.
   * @param right The right (probe) child.
   * @param left_key_expressions The join keys on the left, evaluated against the left child.
   * @param right_key_expressions The join keys on the right, positionally matched to the left's.
   * @param join_type INNER or LEFT.
   */
  HashJoinPlanNode(SchemaRef output_schema, AbstractPlanNodeRef left, AbstractPlanNodeRef right,
                   std::vector<AbstractExpressionRef> left_key_expressions,
                   std::vector<AbstractExpressionRef> right_key_expressions, JoinType join_type)
      : AbstractPlanNode(std::move(output_schema), {std::move(left), std::move(right)}),
        left_key_expressions_{std::move(left_key_expressions)},
        right_key_expressions_{std::move(right_key_expressions)},
        join_type_(join_type) {}

  auto GetType() const -> PlanType override { return PlanType::HashJoin; }

  /** @return The join keys on the left. */
  auto LeftJoinKeyExpressions() const -> const std::vector<AbstractExpressionRef> & {
    return left_key_expressions_;
  }

  /** @return The join keys on the right. */
  auto RightJoinKeyExpressions() const -> const std::vector<AbstractExpressionRef> & {
    return right_key_expressions_;
  }

  /** @return The left (build) child. */
  auto GetLeftPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 2, "Hash joins should have exactly two children.");
    return GetChildAt(0);
  }

  /** @return The right (probe) child. */
  auto GetRightPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 2, "Hash joins should have exactly two children.");
    return GetChildAt(1);
  }

  /** @return The join type. */
  auto GetJoinType() const -> JoinType { return join_type_; }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(HashJoinPlanNode);

  /** The join keys on the left. */
  std::vector<AbstractExpressionRef> left_key_expressions_;
  /** The join keys on the right, positionally matched to the left's. */
  std::vector<AbstractExpressionRef> right_key_expressions_;
  /** The join type. */
  JoinType join_type_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
