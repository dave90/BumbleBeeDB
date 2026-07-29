//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// planner.h
//
// Identification: src/include/planner/planner.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "binder/table_ref/bound_subquery_ref.h"
#include "binder/tokens.h"
#include "catalog/catalog.h"
#include "catalog/column.h"
#include "common/exception.h"
#include "common/macros.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/aggregation_plan.h"
#include "type/value.h"

namespace bumblebee {

class BoundStatement;
class SelectStatement;
class InsertStatement;
class DeleteStatement;
class UpdateStatement;
class BoundExpression;
class BoundTableRef;
class BoundBinaryOp;
class BoundConstant;
class BoundColumnRef;
class BoundUnaryOp;
class BoundBaseTableRef;
class BoundSubqueryRef;
class BoundCrossProductRef;
class BoundJoinRef;
class BoundExpressionListRef;
class BoundAggCall;
class BoundCTERef;
class BoundFuncCall;
class BoundSubqueryExpr;
class ColumnValueExpression;

/**
 * The planner's scope. Chiefly used to plan aggregation, which is a two-pass affair.
 */
class PlannerContext {
 public:
  PlannerContext() = default;

  /**
   * @brief Record an aggregate call found while planning an expression.
   *
   * @param expr The aggregate call.
   */
  void AddAggregation(std::unique_ptr<BoundExpression> expr);

  /** True if an aggregate call is legal at this position. */
  bool allow_aggregation_{false};

  /** The index of the next aggregate call to be planned. */
  size_t next_aggregation_{0};

  /**
   * First pass: every aggregate call found in the select list and HAVING clause is
   * collected here. These expressions are evaluated against the aggregation node's
   * *input*.
   */
  std::vector<std::unique_ptr<BoundExpression>> aggregations_;

  /**
   * Second pass: `aggregations_` is planned into an AggregationPlanNode, and each
   * aggregate call is replaced by a reference to the corresponding output column of
   * that node. These expressions are evaluated against the aggregation node's *output*.
   */
  std::vector<AbstractExpressionRef> expr_in_agg_;

  /** The CTEs visible at this point. */
  const CTEList *cte_list_{nullptr};
};

/**
 * Turns a bound statement into a plan tree.
 *
 * Each `PlanXXX` corresponds to a `BoundXXX` produced by the binder.
 */
class Planner {
 public:
  /**
   * @brief Construct a planner.
   *
   * @param catalog The catalog. It must outlive the planner.
   */
  explicit Planner(const Catalog &catalog) : catalog_(catalog) {}

  /**
   * @brief Plan a statement, leaving the result in `plan_`.
   *
   * @param statement The bound statement.
   */
  void PlanQuery(const BoundStatement &statement);

  auto PlanSelect(const SelectStatement &statement) -> AbstractPlanNodeRef;

  auto PlanTableRef(const BoundTableRef &table_ref) -> AbstractPlanNodeRef;

  auto PlanSubquery(const BoundSubqueryRef &table_ref, const std::string &alias) -> AbstractPlanNodeRef;

  auto PlanBaseTableRef(const BoundBaseTableRef &table_ref) -> AbstractPlanNodeRef;

  auto PlanCrossProductRef(const BoundCrossProductRef &table_ref) -> AbstractPlanNodeRef;

  auto PlanJoinRef(const BoundJoinRef &table_ref) -> AbstractPlanNodeRef;

  auto PlanCTERef(const BoundCTERef &table_ref) -> AbstractPlanNodeRef;

  auto PlanExpressionListRef(const BoundExpressionListRef &table_ref) -> AbstractPlanNodeRef;

  void AddAggCallToContext(BoundExpression &expr);

  auto PlanExpression(const BoundExpression &expr, const std::vector<AbstractPlanNodeRef> &children)
      -> std::tuple<std::string, AbstractExpressionRef>;

  auto PlanBinaryOp(const BoundBinaryOp &expr, const std::vector<AbstractPlanNodeRef> &children)
      -> AbstractExpressionRef;

  auto PlanFuncCall(const BoundFuncCall &expr, const std::vector<AbstractPlanNodeRef> &children)
      -> AbstractExpressionRef;

  auto PlanColumnRef(const BoundColumnRef &expr, const std::vector<AbstractPlanNodeRef> &children)
      -> std::tuple<std::string, std::shared_ptr<ColumnValueExpression>>;

  auto PlanConstant(const BoundConstant &expr, const std::vector<AbstractPlanNodeRef> &children)
      -> AbstractExpressionRef;

  auto PlanSelectAgg(const SelectStatement &statement, AbstractPlanNodeRef child) -> AbstractPlanNodeRef;

  /**
   * @brief Apply a bound WHERE clause to `plan`, flattening its subquery conjuncts into joins.
   *
   * Shared by SELECT, UPDATE and DELETE so all three treat a WHERE the same way: ordinary conjuncts
   * become one Filter directly over `plan`, each IN/EXISTS conjunct stacks a SEMI/ANTI join on top,
   * and (SELECT only) each correlated scalar decorrelates into a LEFT join plus its own filter.
   *
   * @param where The WHERE clause. Must not be the INVALID sentinel.
   * @param plan The plan the clause restricts.
   * @param outer_statement The enclosing SELECT, or null for an UPDATE/DELETE — which disables the
   *        correlated-scalar rewrite (it needs the statement) and leaves those conjuncts to fail
   *        with the planner's usual unsupported-correlation error.
   * @return AbstractPlanNodeRef The plan with the clause applied.
   */
  auto PlanWhere(const BoundExpression &where, AbstractPlanNodeRef plan,
                 const SelectStatement *outer_statement) -> AbstractPlanNodeRef;

  /**
   * @brief Flatten an IN/EXISTS subquery WHERE-conjunct into the plan.
   *
   * `x [NOT] IN (SELECT ...)` becomes a SEMI/ANTI hash join of `outer` against the subquery
   * (null-aware, so NOT IN's three-valued semantics hold); an uncorrelated `[NOT] EXISTS` is
   * pre-executed to a row count (or kept behind a placeholder filter on the EXPLAIN path).
   *
   * @param subquery The bound subquery conjunct (kind ANY or EXISTS).
   * @param outer The plan the conjunct filters.
   * @return AbstractPlanNodeRef The plan with the conjunct applied.
   */
  auto PlanSubqueryPredicate(const BoundSubqueryExpr &subquery, AbstractPlanNodeRef outer) -> AbstractPlanNodeRef;

  /**
   * @brief Decorrelate a correlated scalar-aggregate subquery into a LEFT join on `outer`.
   *
   * `(SELECT agg(...) FROM ... WHERE inner_col = outer_col AND rest)` becomes
   * `outer LEFT JOIN (SELECT inner_col, agg(...) FROM ... WHERE rest GROUP BY inner_col)` on the
   * correlation keys; the subquery expression then resolves (via `resolved_subqueries_`) to the
   * aggregate column of the join output. LEFT so an outer row with no group sees NULL, matching
   * scalar-subquery semantics.
   *
   * @param subquery The correlated scalar subquery (kind SCALAR, correlated).
   * @param outer The enclosing plan.
   * @param outer_statement The enclosing SELECT, source of the correlation-key restriction.
   * @return AbstractPlanNodeRef The join.
   */
  auto PlanCorrelatedScalarSubquery(const BoundSubqueryExpr &subquery, AbstractPlanNodeRef outer,
                                    const SelectStatement &outer_statement) -> AbstractPlanNodeRef;

  /**
   * @brief Build the plan producing a superset of the correlation-key values the outer query
   * will probe with, or null when no useful restriction exists.
   *
   * Decorrelation groups the WHOLE correlated table by the correlation key, even though the outer
   * query only ever probes the few keys its own filters leave alive (TPC-H q17: 1k parts out of
   * 1M, yet the subquery aggregates 30M lineitems into 1M groups). This is the source side of the
   * fix: `<key column> FROM <the base table it comes from> WHERE <the outer conjuncts that mention
   * only that table>` — a superset of the probed keys, because the outer plan can only ever apply
   * MORE restrictions (further conjuncts, joins) on top.
   *
   * @param outer_statement The enclosing SELECT.
   * @param outer_col The outer side of the correlation equality.
   * @param inner_rows Estimated rows of the correlated table, for the cost guard (0 = unknown).
   * @param[out] key_col_idx The key's column index in the returned plan's schema.
   * @return AbstractPlanNodeRef The key source, or null when the restriction is not worth it.
   */
  auto BuildCorrelationKeySource(const SelectStatement &outer_statement, const BoundColumnRef &outer_col,
                                 idx_t inner_rows, uint32_t *key_col_idx) -> AbstractPlanNodeRef;

  /** @return The table's estimated row count, or 0 when the catalog has no figure for it. */
  auto EstimatedTableRows(const std::string &table_name) const -> idx_t;

  auto PlanAggCall(const BoundAggCall &agg_call, const std::vector<AbstractPlanNodeRef> &children)
      -> std::tuple<AggregationType, std::vector<AbstractExpressionRef>>;

  auto GetAggCallFromFactory(const std::string &func_name, std::vector<AbstractExpressionRef> args)
      -> std::tuple<AggregationType, std::vector<AbstractExpressionRef>>;

  auto GetBinaryExpressionFromFactory(const std::string &op_name, AbstractExpressionRef left,
                                      AbstractExpressionRef right) -> AbstractExpressionRef;

  auto GetFuncCallFromFactory(const std::string &func_name, std::vector<AbstractExpressionRef> args)
      -> AbstractExpressionRef;

  auto PlanInsert(const InsertStatement &statement) -> AbstractPlanNodeRef;

  auto PlanDelete(const DeleteStatement &statement) -> AbstractPlanNodeRef;

  auto PlanUpdate(const UpdateStatement &statement) -> AbstractPlanNodeRef;

  /** The root of the plan tree produced by PlanQuery(). */
  AbstractPlanNodeRef plan_;

  /**
   * Evaluates a planned uncorrelated scalar subquery to its single value (executing it in the
   * caller's transaction context). Set by the driver before PlanQuery on the execution path;
   * left empty on the EXPLAIN path, where the planner emits a SubqueryExpression placeholder
   * instead so nothing runs.
   */
  std::function<Value(const AbstractPlanNodeRef &)> subquery_eval_;

  /**
   * Correlated scalar subqueries that decorrelation has already turned into a join: keyed by the
   * bound subquery node, valued by the expression (a column of the join output) that stands in for
   * its value. Consulted by PlanExpression's SUBQUERY case.
   */
  std::unordered_map<const BoundExpression *, AbstractExpressionRef> resolved_subqueries_;

 private:
  PlannerContext ctx_;

  /** Saves and restores the planner scope across a nested plan. */
  class ContextGuard {
   public:
    explicit ContextGuard(PlannerContext *ctx) : old_ctx_(std::move(*ctx)), ctx_ptr_(ctx) {
      *ctx = PlannerContext();
      ctx->cte_list_ = old_ctx_.cte_list_;
    }
    ~ContextGuard() { *ctx_ptr_ = std::move(old_ctx_); }

    DISALLOW_COPY_AND_MOVE(ContextGuard);

   private:
    PlannerContext old_ctx_;
    PlannerContext *ctx_ptr_;
  };

  /**
   * Any function that changes the scope MUST hold this guard, so that the scope is
   * restored when it returns.
   */
  auto NewContext() -> ContextGuard { return ContextGuard(&ctx_); }

  /**
   * @brief Build an output schema from (name, type) pairs.
   *
   * @param exprs The output columns.
   * @return SchemaRef The schema.
   */
  auto MakeOutputSchema(const std::vector<std::pair<std::string, LogicalType>> &exprs) -> SchemaRef;

  /** The catalog. It must outlive the planner. */
  const Catalog &catalog_;

  /** Supplies a unique id to everything that needs a name and hasn't got one. */
  size_t universal_id_{0};
};

/** The name given to a select-list item that has no name of its own. */
static constexpr const char *const UNNAMED_COLUMN = "<unnamed>";

}  // namespace bumblebee
