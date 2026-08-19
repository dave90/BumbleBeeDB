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

static auto Combine(Side a, Side b) -> Side {
  if (a == Side::NEITHER) {
    return b;
  }
  if (b == Side::NEITHER) {
    return a;
  }
  return a == b ? a : Side::BOTH;
}

/**
 * @brief Which side of a join `expr` reads from, given the left input's width.
 *
 * The predicate here is in *flat* form: every column reference addresses the join's
 * concatenated output schema through tuple 0, so a reference is on the left iff its
 * column index is below the left input's width.
 */
static auto SideOf(const AbstractExpressionRef &expr, size_t left_width) -> Side {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    return col->GetColIdx() < left_width ? Side::LEFT : Side::RIGHT;
  }
  auto side = Side::NEITHER;
  for (const auto &child : expr->GetChildren()) {
    side = Combine(side, SideOf(child, left_width));
  }
  return side;
}

/** @brief Break a predicate into its AND-ed conjuncts. */
static void SplitConjuncts(const AbstractExpressionRef &expr, std::vector<AbstractExpressionRef> &out) {
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expr.get());
      logic != nullptr && logic->logic_type_ == LogicType::And) {
    SplitConjuncts(logic->GetChildAt(0), out);
    SplitConjuncts(logic->GetChildAt(1), out);
    return;
  }
  out.push_back(expr);
}

/** @brief Rebuild a single predicate by AND-ing `conjuncts` back together. */
static auto Conjoin(const std::vector<AbstractExpressionRef> &conjuncts) -> AbstractExpressionRef {
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
 * @brief Rewrite a join's two-sided predicate into flat form.
 *
 * Inside a join a right-input column is `#1.k`; in the join's flat output schema the
 * same column is `#0.(left_width + k)`. This is the inverse of
 * `RewriteExpressionForJoin`, and it lets a join predicate be pushed further down (or
 * re-split) using the same flat addressing a Filter uses.
 */
static auto FlattenJoinPredicate(const AbstractExpressionRef &expr, size_t left_width) -> AbstractExpressionRef {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    auto flat_idx = col->GetTupleIdx() == 0 ? col->GetColIdx() : left_width + col->GetColIdx();
    return std::make_shared<ColumnValueExpression>(0, flat_idx, col->GetReturnType());
  }
  std::vector<AbstractExpressionRef> children;
  children.reserve(expr->GetChildren().size());
  for (const auto &child : expr->GetChildren()) {
    children.emplace_back(FlattenJoinPredicate(child, left_width));
  }
  return expr->CloneWithChildren(children);
}

/**
 * @brief Re-index a right-side conjunct so it addresses the right input directly.
 *
 * A conjunct that reads only the right input still carries the join's flat column
 * indices (`left_width .. left_width + right_width`). Once it sits below the join, on
 * top of the right input alone, each index drops by the left input's width.
 */
static auto RebaseRightToChild(const AbstractExpressionRef &expr, size_t left_width) -> AbstractExpressionRef {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    return std::make_shared<ColumnValueExpression>(0, col->GetColIdx() - left_width, col->GetReturnType());
  }
  std::vector<AbstractExpressionRef> children;
  children.reserve(expr->GetChildren().size());
  for (const auto &child : expr->GetChildren()) {
    children.emplace_back(RebaseRightToChild(child, left_width));
  }
  return expr->CloneWithChildren(children);
}

/**
 * @brief Push `pending` predicates into `node`, threading them as deep as they belong.
 *
 * `pending` is a list of conjuncts in `node`'s flat output schema (every reference is
 * `#0.k`) that must hold at or below `node`. The rule distributes them:
 *
 *  - At an INNER join, each conjunct is classified against the join's two inputs. A
 *    single-side conjunct is pushed into that input (recursively — so it keeps
 *    descending toward the scans); a genuine two-input conjunct becomes the join's own
 *    predicate (re-indexed back to the two-sided `#0.k` / `#1.k` form a join needs).
 *    The join's *own* predicate is flattened into the same pool first, so a cross
 *    product carrying an equi-condition (the shape MergeFilterNLJ leaves behind at
 *    every level of a multi-table join) is split exactly like a pushed-down filter —
 *    and, crucially, each join is rewritten *once*, so the re-indexing never compounds.
 *
 *  - At anything else (a scan, an outer join, an aggregate) the remaining conjuncts
 *    can go no lower: the subtree is optimized on its own and, if any conjuncts are
 *    left, a Filter carrying them is placed on top. Stopping at an outer join is what
 *    keeps the rewrite correct — a predicate must not cross the null-extended side.
 */
auto Optimizer::FilterPushDownInto(const AbstractPlanNodeRef &node, const std::vector<AbstractExpressionRef> &pending)
    -> AbstractPlanNodeRef {
  if (node->GetType() == PlanType::NestedLoopJoin) {
    if (const auto &nlj = dynamic_cast<const NestedLoopJoinPlanNode &>(*node); nlj.GetJoinType() == JoinType::INNER) {
      const auto left_width = nlj.GetLeftPlan()->OutputSchema().GetColumnCount();
      const auto right_width = nlj.GetRightPlan()->OutputSchema().GetColumnCount();

      // Everything to distribute at this join: the pushed-down conjuncts plus the
      // join's own predicate, flattened into the same flat addressing.
      std::vector<AbstractExpressionRef> conjuncts = pending;
      if (!IsPredicateTrue(nlj.Predicate())) {
        SplitConjuncts(FlattenJoinPredicate(nlj.Predicate(), left_width), conjuncts);
      }

      std::vector<AbstractExpressionRef> left_conjuncts;
      std::vector<AbstractExpressionRef> right_conjuncts;
      std::vector<AbstractExpressionRef> join_conjuncts;
      for (const auto &conjunct : conjuncts) {
        switch (SideOf(conjunct, left_width)) {
          case Side::LEFT:
            // Already flat over the left input's schema — no re-indexing needed.
            left_conjuncts.push_back(conjunct);
            break;
          case Side::RIGHT:
            right_conjuncts.push_back(RebaseRightToChild(conjunct, left_width));
            break;
          case Side::NEITHER:
          case Side::BOTH:
            join_conjuncts.push_back(conjunct);
            break;
        }
      }

      auto left = FilterPushDownInto(nlj.GetLeftPlan(), left_conjuncts);
      auto right = FilterPushDownInto(nlj.GetRightPlan(), right_conjuncts);
      // Re-index the two-input conjuncts back into the join's `#0.k` / `#1.k` form.
      auto predicate = RewriteExpressionForJoin(Conjoin(join_conjuncts), left_width, right_width);
      return std::make_shared<NestedLoopJoinPlanNode>(nlj.output_schema_, std::move(left), std::move(right),
                                                      std::move(predicate), JoinType::INNER);
    }
  }

  // Not an inner join: the pending conjuncts can descend no further. Optimize the
  // subtree structurally, then re-apply what is left as a Filter on top.
  auto optimized = OptimizeFilterPushDown(node);
  if (pending.empty()) {
    return optimized;
  }
  return std::make_shared<FilterPlanNode>(std::make_shared<Schema>(optimized->OutputSchema()), Conjoin(pending),
                                          std::move(optimized));
}

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
 * pushed into that side (a Filter above a scan, which MergeFilterScan folds in); what
 * remains is a pure equi-join predicate that NLJAsHashJoin accepts. The push is
 * *recursive*: in a multi-table join the planner nests cross products, and
 * MergeFilterNLJ folds the WHERE only into the outermost one, so every inner join
 * arrives as a bare cross product. `FilterPushDownInto` descends through all of them,
 * settling each equi-condition onto its own join and each single-table filter onto its
 * scan — turning what was one hash join over a stack of cross products into a hash join
 * at every level.
 *
 * Must run after MergeFilterNLJ, which is what puts the top WHERE clause into the join
 * in the first place, and before NLJAsHashJoin, which is what benefits.
 *
 * Only INNER joins are eligible: pushing a predicate through the null-extended side
 * of an outer join changes the answer, since rows filtered out below would have been
 * null-padded back in above.
 */
auto Optimizer::OptimizeFilterPushDown(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  // An inner join is the entry into the recursive distribution; it owns its whole
  // subtree, so do not also recurse structurally over it.
  if (plan->GetType() == PlanType::NestedLoopJoin) {
    if (const auto &nlj = dynamic_cast<const NestedLoopJoinPlanNode &>(*plan); nlj.GetJoinType() == JoinType::INNER) {
      return FilterPushDownInto(plan, {});
    }
  }

  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeFilterPushDown(child));
  }
  return plan->CloneWithChildren(std::move(children));
}

}  // namespace bumblebee
