//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// filter_push_down.cpp
//
// Identification: src/optimizer/filter_push_down.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <optional>
#include <vector>

#include "catalog/schema.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "optimizer/optimizer.h"
#include "type/value.h"

namespace bumblebee {

namespace {

/** Which of a join's two inputs an expression reads from. */
enum class Side {
  /** Reads no column at all, e.g. a bare constant. */
  NEITHER,
  /** Reads only the left input. */
  LEFT,
  /** Reads only the right input. */
  RIGHT,
  /** Reads both inputs, so it is a genuine join condition. */
  BOTH,
};

auto Combine(Side a, Side b) -> Side {
  if (a == Side::NEITHER) {
    return b;
  }
  if (b == Side::NEITHER) {
    return a;
  }
  return a == b ? a : Side::BOTH;
}

/** @brief Determine which of the join's inputs `expr` reads from. */
auto SideOf(const AbstractExpressionRef &expr) -> Side {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    return col->GetTupleIdx() == 0 ? Side::LEFT : Side::RIGHT;
  }
  auto side = Side::NEITHER;
  for (const auto &child : expr->GetChildren()) {
    side = Combine(side, SideOf(child));
  }
  return side;
}

/** @brief Break a predicate into its AND-ed conjuncts. */
void SplitConjuncts(const AbstractExpressionRef &expr, std::vector<AbstractExpressionRef> &out) {
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expr.get());
      logic != nullptr && logic->logic_type_ == LogicType::And) {
    SplitConjuncts(logic->GetChildAt(0), out);
    SplitConjuncts(logic->GetChildAt(1), out);
    return;
  }
  out.push_back(expr);
}

/** @brief Rebuild a single predicate by AND-ing `conjuncts` back together. */
auto Conjoin(const std::vector<AbstractExpressionRef> &conjuncts) -> AbstractExpressionRef {
  if (conjuncts.empty()) {
    // Nothing left to test: the join is unrestricted. EliminateTrueFilter and the
    // hash-join rule both understand a constant-true predicate.
    return std::make_shared<ConstantValueExpression>(Value{true});
  }
  auto result = conjuncts[0];
  for (size_t i = 1; i < conjuncts.size(); i++) {
    result = std::make_shared<LogicExpression>(result, conjuncts[i], LogicType::And);
  }
  return result;
}

/**
 * @brief Re-index a conjunct so it addresses one join input directly.
 *
 * Inside the join, a column of the right input is `#1.k`. Once the conjunct sits
 * below the join, on top of that input alone, the same column is `#0.k`.
 */
auto RebaseToChild(const AbstractExpressionRef &expr) -> AbstractExpressionRef {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    return std::make_shared<ColumnValueExpression>(0, col->GetColIdx(), col->GetReturnType());
  }
  std::vector<AbstractExpressionRef> children;
  children.reserve(expr->GetChildren().size());
  for (const auto &child : expr->GetChildren()) {
    children.emplace_back(RebaseToChild(child));
  }
  return expr->CloneWithChildren(children);
}

}  // namespace

/**
 * @brief Push the single-table parts of a join predicate down below the join.
 *
 * `SELECT * FROM a, b WHERE a.x = b.p AND a.v > 10` reaches this rule as one
 * nested loop join carrying the whole conjunction. That is bad twice over: the
 * `a.v > 10` test is re-evaluated for every *pair* of rows rather than once per row
 * of `a`, and — worse — NLJAsHashJoin refuses the join outright, because it requires
 * every conjunct to be an equi-join key and `a.v > 10` is not. So a query degrades
 * from a linear hash join to a quadratic nested loop merely for having a WHERE
 * clause alongside its join condition.
 *
 * Splitting the conjunction fixes both. Each conjunct that reads only one side is
 * pushed into a Filter directly above that side (MergeFilterScan then folds it into
 * the scan); what remains is a pure equi-join predicate that NLJAsHashJoin accepts.
 *
 * Must run after MergeFilterNLJ, which is what puts the WHERE clause into the join
 * in the first place, and before NLJAsHashJoin, which is what benefits.
 *
 * Only INNER joins are eligible: pushing a predicate through the null-extended side
 * of an outer join changes the answer, since rows filtered out below would have been
 * null-padded back in above.
 */
auto Optimizer::OptimizeFilterPushDown(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeFilterPushDown(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::NestedLoopJoin) {
    return optimized_plan;
  }

  const auto &nlj_plan = dynamic_cast<const NestedLoopJoinPlanNode &>(*optimized_plan);
  if (nlj_plan.GetJoinType() != JoinType::INNER || nlj_plan.predicate_ == nullptr) {
    return optimized_plan;
  }

  std::vector<AbstractExpressionRef> conjuncts;
  SplitConjuncts(nlj_plan.predicate_, conjuncts);

  std::vector<AbstractExpressionRef> left_conjuncts;
  std::vector<AbstractExpressionRef> right_conjuncts;
  std::vector<AbstractExpressionRef> join_conjuncts;
  for (const auto &conjunct : conjuncts) {
    switch (SideOf(conjunct)) {
      case Side::LEFT:
        left_conjuncts.push_back(conjunct);
        break;
      case Side::RIGHT:
        right_conjuncts.push_back(conjunct);
        break;
      // A constant conjunct stays on the join, where EliminateTrueFilter can see it.
      case Side::NEITHER:
      case Side::BOTH:
        join_conjuncts.push_back(conjunct);
        break;
    }
  }

  if (left_conjuncts.empty() && right_conjuncts.empty()) {
    return optimized_plan;
  }

  auto left = nlj_plan.GetLeftPlan();
  if (!left_conjuncts.empty()) {
    left = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(left->OutputSchema()),
                                            RebaseToChild(Conjoin(left_conjuncts)), left);
  }

  auto right = nlj_plan.GetRightPlan();
  if (!right_conjuncts.empty()) {
    right = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(right->OutputSchema()),
                                             RebaseToChild(Conjoin(right_conjuncts)), right);
  }

  return std::make_shared<NestedLoopJoinPlanNode>(nlj_plan.output_schema_, std::move(left), std::move(right),
                                                  Conjoin(join_conjuncts), nlj_plan.GetJoinType());
}

}  // namespace bumblebee
