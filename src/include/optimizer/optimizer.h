//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// optimizer.h
//
// Identification: src/include/optimizer/optimizer.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"
#include "optimizer/join_order/greedy_join_order.h"

namespace bumblebee {

/**
 * Rewrites a plan tree into an equivalent, cheaper one.
 *
 * Every rule is a bottom-up tree rewrite: it recurses into the children first,
 * then tries to match at the current node. The rules are purely structural — there
 * are no indexes and no table statistics yet, so nothing here is cost-based.
 */
class Optimizer {
 public:
  /**
   * @brief Construct an optimizer.
   *
   * @param catalog The catalog. It must outlive the optimizer.
   */
  explicit Optimizer(const Catalog &catalog) : catalog_(catalog) {}

  /**
   * @brief Rewrite `plan` into an equivalent, cheaper plan.
   *
   * @param plan The plan to optimize.
   * @return AbstractPlanNodeRef The optimized plan.
   */
  auto Optimize(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

 private:
  /** Collapse a projection that just re-emits its child's columns unchanged. */
  auto OptimizeMergeProjection(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /** Pull a Filter sitting on a cross-product NestedLoopJoin into the join's predicate. */
  auto OptimizeMergeFilterNLJ(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /** Push the single-table conjuncts of a join predicate down below the join. */
  auto OptimizeFilterPushDown(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /**
   * @brief Recursively distribute `pending` conjuncts into `node`'s subtree.
   *
   * The workhorse of FilterPushDown: at each inner join it sends single-table
   * conjuncts down the matching input and keeps the two-input conjuncts on the join,
   * descending through nested cross products so every level of a multi-table join
   * becomes an equi-join. See the definition for the full contract.
   *
   * @param node The subtree to push into.
   * @param pending Conjuncts, in `node`'s flat output schema, that must hold at or
   *   below `node`.
   * @return AbstractPlanNodeRef The rewritten subtree.
   */
  auto FilterPushDownInto(const AbstractPlanNodeRef &node, const std::vector<AbstractExpressionRef> &pending)
      -> AbstractPlanNodeRef;

  /** Turn a NestedLoopJoin whose predicate is a conjunction of equalities into a HashJoin. */
  auto OptimizeNLJAsHashJoin(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /**
   * @brief Cost-based join reordering: reorder each 3+-table inner-join region (a Filter over a
   * cross-product chain) using `join_order_`, emitting a bushy tree of inner joins with predicates
   * placed on their joins and the smaller side built. Runs before MergeFilterNLJ/NLJAsHashJoin, which
   * then lower the emitted inner joins to hash joins. Regions of ≤2 tables are left untouched.
   */
  auto OptimizeJoinOrder(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /** Drop a Filter whose predicate is the constant `true`. */
  auto OptimizeEliminateTrueFilter(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /** Push a Filter sitting on a SeqScan into the scan's filter predicate. */
  auto OptimizeMergeFilterScan(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /** Collapse a Limit over a Sort into a TopN. */
  auto OptimizeSortLimitAsTopN(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /** Not implemented yet — see the note in column_pruning.cpp. */
  auto OptimizeColumnPruning(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef;

  /**
   * @brief Re-index the column references in a join predicate.
   *
   * The planner emits a cross product plus a filter, so the filter's column
   * references are all against one flat concatenated schema: `#0.0 .. #0.n`. A join
   * addresses its two inputs separately, so the references past the left input's
   * width have to be rewritten to `#1.k`.
   *
   * @param expr The predicate.
   * @param left_column_cnt The number of columns on the left of the join.
   * @param right_column_cnt The number of columns on the right of the join.
   * @return AbstractExpressionRef The re-indexed predicate.
   */
  auto RewriteExpressionForJoin(const AbstractExpressionRef &expr, size_t left_column_cnt, size_t right_column_cnt)
      -> AbstractExpressionRef;

  /** @return True if `expr` is the constant `true`. */
  auto IsPredicateTrue(const AbstractExpressionRef &expr) -> bool;

  /**
   * @brief A rough row count for a table, inferred from its name.
   *
   * There are no statistics, so this reads a `_1k` / `_10k` / `_100k` suffix off the
   * table name. It is the only cardinality signal available, and it is the hook a
   * future join-reordering rule would use.
   *
   * @param table_name The table name.
   * @return std::optional<size_t> The estimated row count, if the name says.
   */
  auto EstimatedCardinality(const std::string &table_name) -> std::optional<size_t>;

  /** The catalog. It must outlive the optimizer. */
  const Catalog &catalog_;

  /** The swappable join-order search (GOO today; a DPccp enumerator could replace it). */
  std::unique_ptr<JoinOrderAlgorithm> join_order_ = std::make_unique<GreedyJoinOrder>();
};

}  // namespace bumblebee
