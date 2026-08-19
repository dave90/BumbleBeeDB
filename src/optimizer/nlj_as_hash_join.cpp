//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// nlj_as_hash_join.cpp
//
// Identification: src/optimizer/nlj_as_hash_join.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <vector>

#include "catalog/schema.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

/**
 * @brief Pull the equi-join keys out of a join predicate.
 *
 * Accepts a conjunction of `<column> = <column>` comparisons where the two sides
 * come from different inputs, and appends each side to the matching key list. Any
 * other shape (a comparison against a constant, a non-equality, an OR) makes the
 * whole predicate unusable as hash keys.
 *
 * @param left_keys Filled with the left-hand key expressions.
 * @param right_keys Filled with the right-hand key expressions, positionally matched.
 * @param predicate The join predicate.
 * @return bool True if the entire predicate reduced to equi-join keys.
 */
static auto ExtractKeys(std::vector<AbstractExpressionRef> &left_keys, std::vector<AbstractExpressionRef> &right_keys,
                        const AbstractExpressionRef &predicate) -> bool {
  if (const auto *logic = dynamic_cast<const LogicExpression *>(predicate.get()); logic != nullptr) {
    // The caller has already rejected predicates containing an OR.
    BUMBLEBEE_ASSERT(logic->logic_type_ == LogicType::And, "OR in a join predicate reached ExtractKeys");
    auto result = true;
    for (const auto &child : logic->children_) {
      result &= ExtractKeys(left_keys, right_keys, child);
    }
    return result;
  }

  const auto *cmp = dynamic_cast<const ComparisonExpression *>(predicate.get());
  if (cmp == nullptr || cmp->comp_type_ != ComparisonType::Equal) {
    return false;
  }
  BUMBLEBEE_ASSERT(cmp->GetChildren().size() == 2, "a comparison must have exactly two children");

  const auto *lhs = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(0).get());
  const auto *rhs = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(1).get());
  // Both sides must be columns, and they must come from *different* inputs — an
  // equality between two columns of the same side is a filter, not a join key.
  if (lhs == nullptr || rhs == nullptr || lhs->GetTupleIdx() == rhs->GetTupleIdx()) {
    return false;
  }

  // The predicate may be written either way round; normalize so the left key is
  // always the one addressing input 0.
  if (lhs->GetTupleIdx() == 0) {
    left_keys.push_back(cmp->GetChildAt(0));
    right_keys.push_back(cmp->GetChildAt(1));
  } else {
    left_keys.push_back(cmp->GetChildAt(1));
    right_keys.push_back(cmp->GetChildAt(0));
  }
  return true;
}

/**
 * @brief Turn a NestedLoopJoin whose predicate is a conjunction of equalities into a HashJoin.
 *
 * A nested loop join is quadratic; a hash join is linear in the size of the probe
 * side. Any number of AND-ed equi-conditions is supported. A disjunction is not:
 * `a.x = b.x OR a.y = b.y` cannot be answered from a single hash table, so such a
 * join is left alone.
 */
auto Optimizer::OptimizeNLJAsHashJoin(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeNLJAsHashJoin(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::NestedLoopJoin) {
    return optimized_plan;
  }

  const auto &nlj_plan = dynamic_cast<const NestedLoopJoinPlanNode &>(*optimized_plan);
  BUMBLEBEE_ENSURE(nlj_plan.children_.size() == 2, "NestedLoopJoin should have exactly two children.");

  if (nlj_plan.predicate_ == nullptr || LogicExpression::HasOrPredicate(nlj_plan.predicate_)) {
    return optimized_plan;
  }

  std::vector<AbstractExpressionRef> left_keys;
  std::vector<AbstractExpressionRef> right_keys;
  if (!ExtractKeys(left_keys, right_keys, nlj_plan.predicate_) || left_keys.empty()) {
    return optimized_plan;
  }

  return std::make_shared<HashJoinPlanNode>(std::make_shared<Schema>(nlj_plan.OutputSchema()), nlj_plan.children_[0],
                                            nlj_plan.children_[1], left_keys, right_keys, nlj_plan.GetJoinType());
}

}  // namespace bumblebee
