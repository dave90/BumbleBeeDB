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

#include <memory>
#include <string>
#include <tuple>
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
