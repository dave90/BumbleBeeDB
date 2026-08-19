//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// plan_select.cpp
//
// Identification: src/planner/plan_select.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/bound_order_by.h"
#include "binder/bound_table_ref.h"
#include "binder/expressions/bound_agg_call.h"
#include "binder/expressions/bound_alias.h"
#include "binder/expressions/bound_binary_op.h"
#include "binder/expressions/bound_column_ref.h"
#include "binder/expressions/bound_constant.h"
#include "binder/expressions/bound_func_call.h"
#include "binder/expressions/bound_in_expr.h"
#include "binder/expressions/bound_outer_column_ref.h"
#include "binder/expressions/bound_subquery_expr.h"
#include "binder/expressions/bound_type_cast.h"
#include "binder/expressions/bound_unary_op.h"
#include "binder/statement/select_statement.h"
#include "binder/table_ref/bound_base_table_ref.h"
#include "binder/table_ref/bound_cross_product_ref.h"
#include "binder/table_ref/bound_join_ref.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/expressions/subquery_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/values_plan.h"
#include "fmt/format.h"
#include "planner/planner.h"
#include "type/value.h"

namespace bumblebee {

/** @brief Break a bound WHERE clause into its AND-ed conjuncts. */
static void SplitBoundConjuncts(const BoundExpression &expr, std::vector<const BoundExpression *> &out) {
  if (expr.type_ == ExpressionType::BINARY_OP) {
    if (const auto &binary_op = dynamic_cast<const BoundBinaryOp &>(expr); binary_op.op_name_ == "and") {
      SplitBoundConjuncts(*binary_op.larg_, out);
      SplitBoundConjuncts(*binary_op.rarg_, out);
      return;
    }
  }
  out.push_back(&expr);
}

/** @brief True for an IN/EXISTS subquery conjunct, which plans as a join rather than a predicate. */
static auto IsSubqueryPredicate(const BoundExpression &expr) -> bool {
  if (expr.type_ != ExpressionType::SUBQUERY) {
    return false;
  }
  const auto &subquery = dynamic_cast<const BoundSubqueryExpr &>(expr);
  return subquery.kind_ == SubqueryKind::ANY || subquery.kind_ == SubqueryKind::EXISTS;
}

/**
 * @brief Invoke `fn(child)` on each direct sub-expression of `expr`.
 *
 * The three walks below — "does this mention an outer column", "collect the correlated scalars",
 * "is this a plain re-evaluable predicate" — differ only in what they do at a node, not in the
 * shape of the tree. Keeping the shape here means a new ExpressionType is taught to descend once
 * instead of being silently dropped by whichever switch forgot it.
 *
 * @param expr The expression whose children to visit.
 * @param fn Invoked once per direct child.
 * @return bool True if `expr` is a composite node, i.e. its children were visited; false for a
 *         leaf (constant, column ref, star) or a node this walk deliberately does not descend.
 */
template <class FN>
static auto VisitChildren(const BoundExpression &expr, FN &&fn) -> bool {
  switch (expr.type_) {
    case ExpressionType::BINARY_OP: {
      const auto &binary_op = dynamic_cast<const BoundBinaryOp &>(expr);
      fn(*binary_op.larg_);
      fn(*binary_op.rarg_);
      return true;
    }
    case ExpressionType::UNARY_OP:
      fn(*dynamic_cast<const BoundUnaryOp &>(expr).arg_);
      return true;
    case ExpressionType::ALIAS:
      fn(*dynamic_cast<const BoundAlias &>(expr).child_);
      return true;
    case ExpressionType::TYPE_CAST:
      fn(*dynamic_cast<const BoundTypeCast &>(expr).child_);
      return true;
    case ExpressionType::FUNC_CALL:
      for (const auto &arg : dynamic_cast<const BoundFuncCall &>(expr).args_) {
        fn(*arg);
      }
      return true;
    case ExpressionType::AGG_CALL:
      for (const auto &arg : dynamic_cast<const BoundAggCall &>(expr).args_) {
        fn(*arg);
      }
      return true;
    case ExpressionType::IN_EXPR: {
      const auto &in_expr = dynamic_cast<const BoundInExpr &>(expr);
      fn(*in_expr.child_);
      for (const auto &item : in_expr.list_) {
        fn(*item);
      }
      return true;
    }
    default:
      // Leaves (constant, in-scope column ref, star, outer column ref) and SUBQUERY, whose
      // internals belong to the subquery — every caller below decides about it for itself.
      return false;
  }
}

/** @brief Does this expression contain a reference to an ENCLOSING query's column? A nested
 * subquery counts through its correlated flag (its own internals are its own business). */
static auto ContainsOuterRef(const BoundExpression &expr) -> bool {
  if (expr.type_ == ExpressionType::OUTER_COLUMN_REF) {
    return true;
  }
  if (expr.type_ == ExpressionType::SUBQUERY) {
    return dynamic_cast<const BoundSubqueryExpr &>(expr).correlated_;
  }
  bool found = false;
  VisitChildren(expr, [&found](const BoundExpression &child) { found = found || ContainsOuterRef(child); });
  return found;
}

/** @brief Collect the correlated scalar subqueries inside `expr` (without descending into nested
 * subqueries — theirs are their own). */
static void CollectCorrelatedScalars(const BoundExpression &expr, std::vector<const BoundSubqueryExpr *> &out) {
  if (expr.type_ == ExpressionType::SUBQUERY) {
    const auto &subquery = dynamic_cast<const BoundSubqueryExpr &>(expr);
    if (subquery.kind_ == SubqueryKind::SCALAR && subquery.correlated_) {
      out.push_back(&subquery);
    }
    return;
  }
  VisitChildren(expr, [&out](const BoundExpression &child) { CollectCorrelatedScalars(child, out); });
}

/**
 * @brief Collect every column reference in `expr`, or fail if it holds something a single-table
 * filter cannot carry (an aggregate, a subquery, an outer reference, a star).
 *
 * @param expr The conjunct.
 * @param out The column references found so far. Meaningless when this returns false.
 * @return bool True if the whole expression is a plain, re-evaluable predicate.
 */
static auto CollectPlainColumnRefs(const BoundExpression &expr, std::vector<const BoundColumnRef *> &out) -> bool {
  switch (expr.type_) {
    case ExpressionType::COLUMN_REF:
      out.push_back(&dynamic_cast<const BoundColumnRef &>(expr));
      return true;
    case ExpressionType::CONSTANT:
      return true;
    case ExpressionType::AGG_CALL:
      // Rejected rather than descended into (which VisitChildren would happily do): an aggregate
      // is not a row-wise predicate, so a filter carrying one cannot be re-evaluated per row.
      return false;
    default:
      break;
  }
  // `ok` is assigned first in the `&&` so the recursion always runs on every child; a composite
  // node is plain only if all of its children are, and any other node is not plain at all.
  bool ok = true;
  const bool composite =
      VisitChildren(expr, [&ok, &out](const BoundExpression &child) { ok = CollectPlainColumnRefs(child, out) && ok; });
  return composite && ok;
}

/**
 * @brief Find the base table in a FROM tree that supplies the column named `col_name`.
 *
 * Descending through a join of any type is sound for the caller's purpose: whatever the join does,
 * a value of this column in the join's output either came from this base table or is a NULL
 * fill-in, so the base table's values remain a superset.
 *
 * @param table_ref The FROM tree.
 * @param col_name The qualified column name.
 * @return const BoundBaseTableRef* The base table, or null (a subquery/CTE/VALUES supplies it).
 */
static auto FindKeyBaseTable(const BoundTableRef &table_ref, const std::string &col_name) -> const BoundBaseTableRef * {
  switch (table_ref.type_) {
    case TableReferenceType::BASE_TABLE: {
      const auto &base = dynamic_cast<const BoundBaseTableRef &>(table_ref);
      return SeqScanPlanNode::InferScanSchema(base).TryGetColIdx(col_name).has_value() ? &base : nullptr;
    }
    case TableReferenceType::CROSS_PRODUCT: {
      const auto &cross = dynamic_cast<const BoundCrossProductRef &>(table_ref);
      if (const auto *found = FindKeyBaseTable(*cross.left_, col_name); found != nullptr) {
        return found;
      }
      return FindKeyBaseTable(*cross.right_, col_name);
    }
    case TableReferenceType::JOIN: {
      const auto &join = dynamic_cast<const BoundJoinRef &>(table_ref);
      if (const auto *found = FindKeyBaseTable(*join.left_, col_name); found != nullptr) {
        return found;
      }
      return FindKeyBaseTable(*join.right_, col_name);
    }
    default:
      return nullptr;
  }
}

/**
 * @brief Restrict a decorrelated aggregate's input to the rows whose group key is in `key_source`.
 *
 * Splices a SEMI join under the aggregation (its output schema is the left child's, so the
 * aggregation's group/aggregate expressions still index the same columns), rebuilding the
 * projections stacked above it.
 *
 * Sound because the filter is a function of the GROUP BY key alone: a group either keeps every one
 * of its rows or disappears entirely, and the groups that disappear are exactly the ones no outer
 * row could have probed.
 *
 * @param plan The decorrelated subquery plan (an aggregation, possibly under projections).
 * @param group_idx Which group-by key `key_source` restricts.
 * @param key_source The key values.
 * @param key_col_idx The key's column index in `key_source`'s schema.
 * @return AbstractPlanNodeRef The rewritten plan, or null if the shape did not match.
 */
static auto RestrictAggregateInput(const AbstractPlanNodeRef &plan, uint32_t group_idx, AbstractPlanNodeRef key_source,
                                   uint32_t key_col_idx) -> AbstractPlanNodeRef {
  if (plan->GetType() == PlanType::Aggregation) {
    const auto &agg = dynamic_cast<const AggregationPlanNode &>(*plan);
    if (group_idx >= agg.GetGroupBys().size()) {
      return nullptr;
    }
    const auto &group_key = agg.GetGroupByAt(group_idx);
    const auto &key_col = key_source->OutputSchema().GetColumn(key_col_idx);
    // A key type mismatch would make the two sides hash differently; leave those alone.
    if (group_key->GetReturnType().GetType() != key_col.GetType()) {
      return nullptr;
    }
    auto child = agg.GetChildAt(0);
    auto semi_join = std::make_shared<HashJoinPlanNode>(
        child->output_schema_, child, std::move(key_source), std::vector<AbstractExpressionRef>{group_key},
        std::vector<AbstractExpressionRef>{std::make_shared<ColumnValueExpression>(1, key_col_idx, key_col)},
        JoinType::SEMI);
    return plan->CloneWithChildren({std::move(semi_join)});
  }
  if (plan->GetType() == PlanType::Projection && plan->GetChildren().size() == 1) {
    auto rewritten = RestrictAggregateInput(plan->GetChildAt(0), group_idx, std::move(key_source), key_col_idx);
    if (rewritten == nullptr) {
      return nullptr;
    }
    return plan->CloneWithChildren({std::move(rewritten)});
  }
  return nullptr;
}

/** One correlation equality pulled out of a subquery's WHERE: outer column = inner expression. */
struct CorrelationPair {
  /** The outer side, resolved against the ENCLOSING query. */
  std::unique_ptr<BoundColumnRef> outer_;
  /** The inner side, evaluated inside the subquery. */
  std::unique_ptr<BoundExpression> inner_;
};

/**
 * @brief Pull the depth-1 `inner = outer` equalities out of a subquery's WHERE tree.
 *
 * Takes ownership of the AND tree: correlation equalities become `pairs`, everything else lands
 * in `remaining` (a conjunct that still holds an outer ref stays there and fails later with the
 * planner's clean unsupported-correlation error).
 */
static void ExtractCorrelation(std::unique_ptr<BoundExpression> expr, std::vector<CorrelationPair> &pairs,
                               std::vector<std::unique_ptr<BoundExpression>> &remaining) {
  if (expr->type_ == ExpressionType::BINARY_OP) {
    auto &binary_op = dynamic_cast<BoundBinaryOp &>(*expr);
    if (binary_op.op_name_ == "and") {
      ExtractCorrelation(std::move(binary_op.larg_), pairs, remaining);
      ExtractCorrelation(std::move(binary_op.rarg_), pairs, remaining);
      return;
    }
    if (binary_op.op_name_ == "=") {
      const bool left_outer = binary_op.larg_->type_ == ExpressionType::OUTER_COLUMN_REF;
      const bool right_outer = binary_op.rarg_->type_ == ExpressionType::OUTER_COLUMN_REF;
      if (left_outer != right_outer) {
        auto &outer_side = left_outer ? binary_op.larg_ : binary_op.rarg_;
        auto &inner_side = left_outer ? binary_op.rarg_ : binary_op.larg_;
        auto &outer_ref = dynamic_cast<BoundOuterColumnRef &>(*outer_side);
        if (outer_ref.depth_ == 1 && !ContainsOuterRef(*inner_side)) {
          pairs.push_back(CorrelationPair{std::move(outer_ref.inner_), std::move(inner_side)});
          return;
        }
      }
    }
  }
  remaining.push_back(std::move(expr));
}

/** @brief Rebuild a WHERE clause from conjuncts; empty means "no clause" (the INVALID sentinel). */
static auto ConjoinBound(std::vector<std::unique_ptr<BoundExpression>> conjuncts) -> std::unique_ptr<BoundExpression> {
  if (conjuncts.empty()) {
    return std::make_unique<BoundExpression>();
  }
  auto result = std::move(conjuncts[0]);
  for (size_t i = 1; i < conjuncts.size(); i++) {
    result = std::make_unique<BoundBinaryOp>("and", std::move(result), std::move(conjuncts[i]));
  }
  return result;
}

/**
 * @brief Read a LIMIT / OFFSET clause, which must be an integer literal.
 *
 * @param expr The bound clause.
 * @param clause_name "LIMIT" or "OFFSET", for the error message.
 * @return std::optional<size_t> The value, or nullopt if the clause is absent.
 */
static auto PlanLimitValue(const BoundExpression &expr, const char *clause_name) -> std::optional<size_t> {
  if (expr.IsInvalid()) {
    return std::nullopt;
  }
  if (expr.type_ == ExpressionType::CONSTANT) {
    const auto &constant_expr = dynamic_cast<const BoundConstant &>(expr);
    if (!constant_expr.val_.IsNull() && constant_expr.val_.GetType() == LogicalType(LogicalTypeId::INTEGER)) {
      return std::make_optional(static_cast<size_t>(constant_expr.val_.GetAs<int32_t>()));
    }
  }
  throw NotImplementedException(fmt::format("the {} clause must be an integer constant", clause_name));
}

auto Planner::PlanSelect(const SelectStatement &statement) -> AbstractPlanNodeRef {
  auto ctx_guard = NewContext();
  if (!statement.ctes_.empty()) {
    ctx_.cte_list_ = &statement.ctes_;
  }

  AbstractPlanNodeRef plan = nullptr;

  switch (statement.table_->type_) {
    case TableReferenceType::EMPTY:
      // `SELECT 1` with no FROM: a single empty row for the projection to evaluate over.
      plan = std::make_shared<ValuesPlanNode>(
          std::make_shared<Schema>(std::vector<Column>{}),
          std::vector<std::vector<AbstractExpressionRef>>{std::vector<AbstractExpressionRef>{}});
      break;
    default:
      plan = PlanTableRef(*statement.table_);
      break;
  }

  if (!statement.where_->IsInvalid()) {
    plan = PlanWhere(*statement.where_, std::move(plan), &statement);
  }

  const bool has_agg = std::any_of(statement.select_list_.begin(), statement.select_list_.end(),
                                   [](const auto &item) { return item->HasAggregation(); });

  if (!statement.having_->IsInvalid() || !statement.group_by_.empty() || has_agg) {
    plan = PlanSelectAgg(statement, std::move(plan));
  } else {
    std::vector<AbstractExpressionRef> exprs;
    std::vector<std::string> column_names;
    for (const auto &item : statement.select_list_) {
      auto [name, expr] = PlanExpression(*item, {plan});
      if (name == UNNAMED_COLUMN) {
        name = fmt::format("__unnamed#{}", universal_id_++);
      }
      exprs.emplace_back(std::move(expr));
      column_names.emplace_back(std::move(name));
    }
    plan = std::make_shared<ProjectionPlanNode>(std::make_shared<Schema>(ProjectionPlanNode::RenameSchema(
                                                    ProjectionPlanNode::InferProjectionSchema(exprs), column_names)),
                                                std::move(exprs), std::move(plan));
  }

  // DISTINCT is a group-by on every output column with no aggregates.
  if (statement.is_distinct_) {
    auto child = std::move(plan);
    std::vector<AbstractExpressionRef> distinct_exprs;
    size_t col_idx = 0;
    for (const auto &col : child->OutputSchema().GetColumns()) {
      distinct_exprs.emplace_back(std::make_shared<ColumnValueExpression>(0, col_idx++, col));
    }
    plan = std::make_shared<AggregationPlanNode>(std::make_shared<Schema>(child->OutputSchema()), child,
                                                 std::move(distinct_exprs), std::vector<AbstractExpressionRef>{},
                                                 std::vector<AggregationType>{});
  }

  if (!statement.sort_.empty()) {
    std::vector<OrderBy> order_bys;
    order_bys.reserve(statement.sort_.size());
    for (const auto &order_by : statement.sort_) {
      auto [_, expr] = PlanExpression(*order_by->expr_, {plan});
      order_bys.emplace_back(order_by->type_, order_by->null_order_, std::move(expr));
    }
    plan = std::make_shared<SortPlanNode>(std::make_shared<Schema>(plan->OutputSchema()), plan, std::move(order_bys));
  }

  const auto limit = PlanLimitValue(*statement.limit_count_, "LIMIT");
  const auto offset = PlanLimitValue(*statement.limit_offset_, "OFFSET");
  if (offset.has_value()) {
    throw NotImplementedException("the OFFSET clause is not supported yet");
  }
  if (limit.has_value()) {
    // A Sort directly under this Limit becomes a TopN in the optimizer.
    plan = std::make_shared<LimitPlanNode>(std::make_shared<Schema>(plan->OutputSchema()), plan, *limit);
  }

  return plan;
}

auto Planner::PlanWhere(const BoundExpression &where, AbstractPlanNodeRef plan, const SelectStatement *outer_statement)
    -> AbstractPlanNodeRef {
  // IN/EXISTS subquery conjuncts plan as SEMI/ANTI joins, not as filter predicates, so the WHERE
  // is split: the ordinary conjuncts stay a Filter (placed FIRST, directly over the FROM plan, so
  // the existing merge/pushdown/hash-join pipeline sees exactly the shape it always has), and each
  // subquery conjunct stacks its join on top.
  std::vector<const BoundExpression *> conjuncts;
  SplitBoundConjuncts(where, conjuncts);
  std::vector<const BoundExpression *> ordinary;
  std::vector<const BoundSubqueryExpr *> subquery_conjuncts;
  std::vector<const BoundExpression *> correlated_scalar_conjuncts;
  for (const auto *conjunct : conjuncts) {
    if (IsSubqueryPredicate(*conjunct)) {
      subquery_conjuncts.push_back(&dynamic_cast<const BoundSubqueryExpr &>(*conjunct));
      continue;
    }
    std::vector<const BoundSubqueryExpr *> scalars;
    CollectCorrelatedScalars(*conjunct, scalars);
    // Decorrelating a scalar needs the enclosing SELECT (it is the source of the correlation-key
    // restriction). Without one — an UPDATE/DELETE WHERE — the conjunct falls through to the
    // ordinary path, where planning its OUTER_COLUMN_REF raises the usual unsupported-correlation
    // error rather than silently producing a wrong plan.
    if (!scalars.empty() && outer_statement != nullptr) {
      correlated_scalar_conjuncts.push_back(conjunct);
    } else {
      ordinary.push_back(conjunct);
    }
  }

  if (!ordinary.empty()) {
    auto schema = plan->OutputSchema();
    AbstractExpressionRef predicate = nullptr;
    for (const auto *conjunct : ordinary) {
      auto [_, expr] = PlanExpression(*conjunct, {plan});
      predicate = predicate == nullptr
                      ? std::move(expr)
                      : std::make_shared<LogicExpression>(std::move(predicate), std::move(expr), LogicType::And);
    }
    plan = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(schema), std::move(predicate), std::move(plan));
  }
  for (const auto *subquery : subquery_conjuncts) {
    plan = PlanSubqueryPredicate(*subquery, std::move(plan));
  }
  // Each correlated scalar decorrelates into a LEFT join (widening the plan with the subquery's
  // key + value columns), then its conjunct filters over the join output. The projection planned
  // by the caller narrows the schema back to the select list.
  for (const auto *conjunct : correlated_scalar_conjuncts) {
    std::vector<const BoundSubqueryExpr *> scalars;
    CollectCorrelatedScalars(*conjunct, scalars);
    for (const auto *scalar : scalars) {
      plan = PlanCorrelatedScalarSubquery(*scalar, std::move(plan), *outer_statement);
    }
    auto [_, expr] = PlanExpression(*conjunct, {plan});
    plan = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(plan->OutputSchema()), std::move(expr),
                                            std::move(plan));
  }
  return plan;
}

auto Planner::PlanSubqueryPredicate(const BoundSubqueryExpr &subquery, AbstractPlanNodeRef outer)
    -> AbstractPlanNodeRef {
  if (subquery.correlated_) {
    if (subquery.kind_ != SubqueryKind::EXISTS) {
      throw NotImplementedException("a correlated IN subquery is not supported (only correlated EXISTS)");
    }
    return PlanCorrelatedExists(subquery, std::move(outer));
  }

  // The subquery plans in its own scope; the eval hook propagates so nested scalar subqueries
  // pre-execute the same way.
  Planner sub_planner(catalog_);
  sub_planner.subquery_eval_ = subquery_eval_;
  sub_planner.PlanQuery(*subquery.subquery_);
  auto subplan = sub_planner.plan_;

  if (subquery.kind_ == SubqueryKind::ANY) {
    // `x [NOT] IN (SELECT y ...)` -> a null-aware SEMI/ANTI hash join: outer probes (left), the
    // subquery builds (right). Output schema = the outer schema, untouched.
    if (subplan->OutputSchema().GetColumnCount() != 1) {
      throw PlannerException("IN subquery must return exactly one column");
    }
    auto [_, tested] = PlanExpression(*subquery.testexpr_, {outer});
    auto right_key = std::make_shared<ColumnValueExpression>(1, 0, subplan->OutputSchema().GetColumn(0));
    auto join = std::make_shared<HashJoinPlanNode>(
        outer->output_schema_, outer, std::move(subplan), std::vector<AbstractExpressionRef>{std::move(tested)},
        std::vector<AbstractExpressionRef>{std::move(right_key)}, subquery.negated_ ? JoinType::ANTI : JoinType::SEMI);
    join->null_aware_ = true;
    return join;
  }

  BUMBLEBEE_ASSERT(subquery.kind_ == SubqueryKind::EXISTS, "unexpected subquery kind");
  return PlanUncorrelatedExists(subquery, std::move(outer), std::move(subplan));
}

auto Planner::PlanCorrelatedExists(const BoundSubqueryExpr &subquery, AbstractPlanNodeRef outer)
    -> AbstractPlanNodeRef {
  // Pull the `inner = outer` equalities out of the subquery WHERE, make the inner sides the
  // subquery's output, and SEMI/ANTI-join the enclosing plan against it on those keys. Any
  // remaining outer reference fails with the planner's clean error.
  auto &sq = *subquery.subquery_;
  // Shapes the join rewrite would silently distort are refused: an ungrouped aggregate
  // subquery yields one row even over zero input (EXISTS would be constantly true), and a
  // LIMIT applies to the whole subquery, not per correlation key.
  const bool has_agg = std::any_of(sq.select_list_.begin(), sq.select_list_.end(),
                                   [](const auto &item) { return item->HasAggregation(); });
  if (has_agg || !sq.group_by_.empty() || !sq.having_->IsInvalid() || !sq.limit_count_->IsInvalid()) {
    throw NotImplementedException(
        "correlated EXISTS does not support aggregation, GROUP BY, HAVING or LIMIT in the subquery");
  }
  std::vector<CorrelationPair> pairs;
  std::vector<std::unique_ptr<BoundExpression>> remaining;
  if (!sq.where_->IsInvalid()) {
    ExtractCorrelation(std::move(sq.where_), pairs, remaining);
  }
  if (pairs.empty()) {
    throw NotImplementedException(
        "correlated EXISTS requires an equality with an outer column in the subquery WHERE clause");
  }
  sq.where_ = ConjoinBound(std::move(remaining));
  sq.select_list_.clear();
  for (auto &pair : pairs) {
    sq.select_list_.push_back(std::move(pair.inner_));
  }

  Planner sub_planner(catalog_);
  sub_planner.subquery_eval_ = subquery_eval_;
  sub_planner.PlanQuery(sq);
  auto subplan = sub_planner.plan_;

  std::vector<AbstractExpressionRef> left_keys;
  std::vector<AbstractExpressionRef> right_keys;
  for (size_t i = 0; i < pairs.size(); i++) {
    auto [_, outer_key] = PlanExpression(*pairs[i].outer_, {outer});
    left_keys.push_back(std::move(outer_key));
    right_keys.push_back(
        std::make_shared<ColumnValueExpression>(1, i, subplan->OutputSchema().GetColumn(static_cast<uint32_t>(i))));
  }
  // EXISTS semantics, NOT null-aware: a NULL key row simply never matches (so NOT EXISTS keeps
  // NULL-keyed outer rows — their per-row subquery result is empty, which is exactly EXISTS=false).
  return std::make_shared<HashJoinPlanNode>(outer->output_schema_, outer, std::move(subplan), std::move(left_keys),
                                            std::move(right_keys), subquery.negated_ ? JoinType::ANTI : JoinType::SEMI);
}

auto Planner::PlanUncorrelatedExists(const BoundSubqueryExpr &subquery, AbstractPlanNodeRef outer,
                                     AbstractPlanNodeRef subplan) -> AbstractPlanNodeRef {
  // The answer is one bit for the whole statement. Reduce the subquery to `count(*) LIMIT 1` and
  // pre-execute it (EXPLAIN keeps it behind a placeholder filter).
  auto limited = std::make_shared<LimitPlanNode>(subplan->output_schema_, std::move(subplan), /*limit=*/1);
  std::vector<AbstractExpressionRef> count_args{
      std::make_shared<ConstantValueExpression>(Value{static_cast<int32_t>(1)})};
  std::vector<AggregationType> count_types{AggregationType::CountStarAggregate};
  auto count_schema = AggregationPlanNode::InferAggSchema({}, count_args, count_types);
  AbstractPlanNodeRef count_plan = std::make_shared<AggregationPlanNode>(
      std::make_shared<Schema>(count_schema), std::move(limited), std::vector<AbstractExpressionRef>{},
      std::move(count_args), std::move(count_types));

  if (!subquery_eval_) {
    // EXPLAIN path: nothing may run — a placeholder filter shows where the EXISTS would apply.
    return std::make_shared<FilterPlanNode>(
        std::make_shared<Schema>(outer->OutputSchema()),
        std::make_shared<SubqueryExpression>(count_plan, Column{"<exists>", LogicalType(LogicalTypeId::BOOLEAN)}),
        std::move(outer));
  }

  const auto count = subquery_eval_(count_plan);
  const bool exists = !count.IsNull() && count.GetAs<int32_t>() > 0;
  if (exists != subquery.negated_) {
    return outer;  // the predicate holds for every row: nothing to add
  }
  // The predicate is constantly false: an always-false filter empties the result.
  return std::make_shared<FilterPlanNode>(std::make_shared<Schema>(outer->OutputSchema()),
                                          std::make_shared<ConstantValueExpression>(Value{false}), std::move(outer));
}

auto Planner::EstimatedTableRows(const std::string &table_name) const -> idx_t {
  if (auto info = catalog_.GetTable(table_name); info != NULL_TABLE_INFO && info->storage_ != nullptr) {
    return info->storage_->EstimatedRowCount();
  }
  return 0;
}

auto Planner::BuildCorrelationKeySource(const SelectStatement &outer_statement, const BoundColumnRef &outer_col,
                                        idx_t inner_rows, uint32_t *key_col_idx) -> AbstractPlanNodeRef {
  /** Assumed selectivity of one single-table conjunct, for the "is this restriction worth an extra
   * scan" guard. Deliberately pessimistic: over-estimating the key count only skips the rewrite. */
  static constexpr double CONJUNCT_SELECTIVITY = 0.2;
  /** The key source must look at least this much smaller than the table it restricts to pay for
   * its own scan. */
  static constexpr double WORTH_A_SCAN_RATIO = 0.5;

  if (outer_statement.where_->IsInvalid()) {
    return nullptr;  // nothing narrows the outer query: the key source would be the whole column
  }
  const auto col_name = outer_col.ToString();
  const auto *base = FindKeyBaseTable(*outer_statement.table_, col_name);
  if (base == nullptr) {
    return nullptr;
  }
  const auto scan_schema = SeqScanPlanNode::InferScanSchema(*base);
  const auto col_idx = scan_schema.TryGetColIdx(col_name);
  if (!col_idx.has_value()) {
    return nullptr;
  }
  const auto table_name = base->GetBoundTableName();

  // The conjuncts that mention this table and nothing else. Anything touching a second table
  // (the join predicate itself, typically) is dropped: the result stays a superset.
  std::vector<const BoundExpression *> conjuncts;
  SplitBoundConjuncts(*outer_statement.where_, conjuncts);
  std::vector<const BoundExpression *> local_conjuncts;
  for (const auto *conjunct : conjuncts) {
    std::vector<const BoundColumnRef *> refs;
    if (!CollectPlainColumnRefs(*conjunct, refs) || refs.empty()) {
      continue;
    }
    const bool all_local = std::all_of(refs.begin(), refs.end(), [&](const auto *ref) {
      return ref->col_name_.size() >= 2 && ref->col_name_.front() == table_name &&
             scan_schema.TryGetColIdx(ref->ToString()).has_value();
    });
    if (all_local) {
      local_conjuncts.push_back(conjunct);
    }
  }
  if (local_conjuncts.empty()) {
    return nullptr;  // an unrestricted key column restricts nothing
  }

  // Cost guard: the extra scan only pays off if the key set is much smaller than the table whose
  // aggregation it prunes. Unknown row counts mean a table too small for either to matter.
  const idx_t key_rows = EstimatedTableRows(base->table_);
  if (key_rows > 0 && inner_rows > 0) {
    const double estimated_keys =
        static_cast<double>(key_rows) * std::pow(CONJUNCT_SELECTIVITY, local_conjuncts.size());
    if (estimated_keys > WORTH_A_SCAN_RATIO * static_cast<double>(inner_rows)) {
      return nullptr;
    }
  }

  AbstractPlanNodeRef scan = PlanBaseTableRef(*base);
  AbstractExpressionRef predicate = nullptr;
  for (const auto *conjunct : local_conjuncts) {
    auto [_, expr] = PlanExpression(*conjunct, {scan});
    predicate = predicate == nullptr
                    ? std::move(expr)
                    : std::make_shared<LogicExpression>(std::move(predicate), std::move(expr), LogicType::And);
  }
  *key_col_idx = *col_idx;
  return std::make_shared<FilterPlanNode>(std::make_shared<Schema>(scan->OutputSchema()), std::move(predicate),
                                          std::move(scan));
}

/**
 * @brief Rewrite a correlated scalar subquery, in place, into the grouped aggregate its
 * decorrelation joins against.
 *
 * The `inner = outer` equalities are pulled out of the WHERE; the inner keys become both the
 * leading output columns and the GROUP BY, ahead of the aggregate. Shapes for which that rewrite
 * would not be sound are refused.
 *
 * @param sq The subquery statement, mutated in place.
 * @return The correlation pairs (outer side untouched, inner side consumed into the rewrite).
 */
static auto RewriteScalarSubqueryForDecorrelation(SelectStatement &sq) -> std::vector<CorrelationPair> {
  // Only the aggregate shape decorrelates soundly: the GROUP BY on the correlation key guarantees
  // one value per key, which is what a scalar subquery must produce per outer row.
  if (sq.select_list_.size() != 1 || !sq.select_list_[0]->HasAggregation() || !sq.group_by_.empty() ||
      !sq.having_->IsInvalid() || !sq.sort_.empty() || !sq.limit_count_->IsInvalid() || sq.is_distinct_) {
    throw NotImplementedException(
        "a correlated scalar subquery must be a single ungrouped aggregate over the correlated table");
  }

  std::vector<CorrelationPair> pairs;
  std::vector<std::unique_ptr<BoundExpression>> remaining;
  if (!sq.where_->IsInvalid()) {
    ExtractCorrelation(std::move(sq.where_), pairs, remaining);
  }
  if (pairs.empty()) {
    throw NotImplementedException(
        "a correlated scalar subquery requires an equality with an outer column in its WHERE clause");
  }
  sq.where_ = ConjoinBound(std::move(remaining));

  // The inner sides become both output columns and GROUP BY keys, so they must be plain columns
  // (a BoundColumnRef is the one bound expression that can be duplicated by name).
  for (auto &pair : pairs) {
    if (pair.inner_->type_ != ExpressionType::COLUMN_REF) {
      throw NotImplementedException("correlation must compare an outer column against a plain subquery column");
    }
  }
  std::vector<std::unique_ptr<BoundExpression>> new_select_list;
  for (auto &pair : pairs) {
    const auto &inner_col = dynamic_cast<const BoundColumnRef &>(*pair.inner_);
    new_select_list.push_back(std::make_unique<BoundColumnRef>(inner_col.col_name_));
    sq.group_by_.push_back(std::make_unique<BoundColumnRef>(inner_col.col_name_));
  }
  new_select_list.push_back(std::move(sq.select_list_[0]));
  sq.select_list_ = std::move(new_select_list);
  return pairs;
}

/** @brief Wrap `subplan` in an identity projection renaming every column to `<tag>.cN` — the
 * subquery usually reads the same tables as the enclosing query, so its column names would
 * otherwise collide in the join schema. */
static auto RenameSubqueryColumns(AbstractPlanNodeRef subplan, const std::string &tag) -> AbstractPlanNodeRef {
  const auto &sub_cols = subplan->OutputSchema().GetColumns();
  std::vector<AbstractExpressionRef> identity;
  std::vector<std::string> names;
  for (uint32_t i = 0; i < sub_cols.size(); i++) {
    identity.push_back(std::make_shared<ColumnValueExpression>(0, i, sub_cols[i]));
    names.push_back(fmt::format("{}.c{}", tag, i));
  }
  auto renamed_schema = ProjectionPlanNode::RenameSchema(ProjectionPlanNode::InferProjectionSchema(identity), names);
  return std::make_shared<ProjectionPlanNode>(std::make_shared<Schema>(renamed_schema), std::move(identity),
                                              std::move(subplan));
}

auto Planner::PlanCorrelatedScalarSubquery(const BoundSubqueryExpr &subquery, AbstractPlanNodeRef outer,
                                           const SelectStatement &outer_statement) -> AbstractPlanNodeRef {
  auto &sq = *subquery.subquery_;
  auto pairs = RewriteScalarSubqueryForDecorrelation(sq);

  Planner sub_planner(catalog_);
  sub_planner.subquery_eval_ = subquery_eval_;
  sub_planner.PlanQuery(sq);
  auto subplan = sub_planner.plan_;

  // Cut the aggregation down to the keys the outer query can actually probe. Restricting on ONE
  // correlation key is enough (and the common case is that there is only one), so the first key
  // with a worthwhile source wins.
  idx_t inner_rows = 0;
  if (sq.table_->type_ == TableReferenceType::BASE_TABLE) {
    inner_rows = EstimatedTableRows(dynamic_cast<const BoundBaseTableRef &>(*sq.table_).table_);
  }
  for (uint32_t i = 0; i < pairs.size(); i++) {
    uint32_t key_col_idx = 0;
    auto key_source = BuildCorrelationKeySource(outer_statement, *pairs[i].outer_, inner_rows, &key_col_idx);
    if (key_source == nullptr) {
      continue;
    }
    if (auto restricted = RestrictAggregateInput(subplan, i, std::move(key_source), key_col_idx);
        restricted != nullptr) {
      subplan = std::move(restricted);
      break;
    }
  }

  auto renamed = RenameSubqueryColumns(std::move(subplan), fmt::format("__corr#{}", universal_id_++));
  const auto &renamed_schema = renamed->OutputSchema();

  std::vector<AbstractExpressionRef> left_keys;
  std::vector<AbstractExpressionRef> right_keys;
  for (size_t i = 0; i < pairs.size(); i++) {
    auto [_, outer_key] = PlanExpression(*pairs[i].outer_, {outer});
    left_keys.push_back(std::move(outer_key));
    right_keys.push_back(
        std::make_shared<ColumnValueExpression>(1, i, renamed_schema.GetColumn(static_cast<uint32_t>(i))));
  }

  // LEFT join: an outer row whose key has no group must see the scalar as NULL, not vanish.
  std::vector<Column> join_cols;
  for (const auto &col : outer->OutputSchema().GetColumns()) {
    join_cols.push_back(col);
  }
  for (const auto &col : renamed_schema.GetColumns()) {
    join_cols.push_back(col);
  }
  const auto outer_width = outer->OutputSchema().GetColumnCount();
  auto join =
      std::make_shared<HashJoinPlanNode>(std::make_shared<Schema>(join_cols), std::move(outer), std::move(renamed),
                                         std::move(left_keys), std::move(right_keys), JoinType::LEFT);

  // From here on, this subquery's value IS the aggregate column of the join output.
  const auto value_idx = outer_width + static_cast<uint32_t>(pairs.size());
  resolved_subqueries_[&subquery] =
      std::make_shared<ColumnValueExpression>(0, value_idx, join->OutputSchema().GetColumn(value_idx));
  return join;
}

}  // namespace bumblebee
