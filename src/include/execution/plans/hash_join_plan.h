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
  /** The join type. SEMI/ANTI emit left rows only (output schema = left schema). */
  JoinType join_type_;
  /**
   * IN / NOT IN three-valued NULL semantics (SEMI/ANTI only): a NULL probe key never qualifies,
   * and a NULL key anywhere in the build side makes NOT IN (ANTI) emit nothing at all. Plain
   * EXISTS / NOT EXISTS joins leave this false: there a NULL key row simply never matches.
   */
  bool null_aware_{false};
  /**
   * Build-side columns some ancestor actually reads (set by OptimizeColumnPruning when
   * `build_live_annotated_`; possibly empty — a SEMI/ANTI join, or a query reading only probe
   * columns, stores nothing beyond the keys). The physical join stores and gathers only these —
   * the other build outputs surface as constant NULLs nothing reads, exactly the scans'
   * convention. Indexes are into the BUILD child's schema (child 0 for INNER, the non-preserved
   * child 1 for LEFT). Un-annotated plans (hand-built, un-optimized) keep the full layout.
   */
  std::vector<idx_t> build_live_columns_;
  bool build_live_annotated_{false};

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
