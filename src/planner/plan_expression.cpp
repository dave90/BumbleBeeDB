//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// plan_expression.cpp
//
// Identification: src/planner/plan_expression.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/expressions/bound_agg_call.h"
#include "binder/expressions/bound_alias.h"
#include "binder/expressions/bound_type_cast.h"
#include "binder/expressions/bound_binary_op.h"
#include "binder/expressions/bound_column_ref.h"
#include "binder/expressions/bound_constant.h"
#include "binder/expressions/bound_func_call.h"
#include "binder/expressions/bound_in_expr.h"
#include "binder/expressions/bound_subquery_expr.h"
#include "binder/expressions/bound_unary_op.h"
#include "common/exception.h"
#include "common/macros.h"
#include "execution/expressions/cast_expression.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/in_expression.h"
#include "execution/expressions/is_null_expression.h"
#include "execution/expressions/subquery_expression.h"
#include "execution/plans/abstract_plan.h"
#include "fmt/format.h"
#include "planner/planner.h"

namespace bumblebee {

auto Planner::PlanBinaryOp(const BoundBinaryOp &expr, const std::vector<AbstractPlanNodeRef> &children)
    -> AbstractExpressionRef {
  auto [_1, left] = PlanExpression(*expr.larg_, children);
  auto [_2, right] = PlanExpression(*expr.rarg_, children);
  return GetBinaryExpressionFromFactory(expr.op_name_, std::move(left), std::move(right));
}

auto Planner::PlanColumnRef(const BoundColumnRef &expr, const std::vector<AbstractPlanNodeRef> &children)
    -> std::tuple<std::string, std::shared_ptr<ColumnValueExpression>> {
  if (children.empty()) {
    throw PlannerException("a column reference needs at least one input to resolve against");
  }

  auto col_name = expr.ToString();

  if (children.size() == 1) {
    // The single-input case: projections, filters, aggregations.
    const auto &schema = children[0]->OutputSchema();
    bool found = false;
    for (const auto &col : schema.GetColumns()) {
      if (col_name == col.GetName()) {
        if (found) {
          throw PlannerException(fmt::format("column {} appears more than once in the input", col_name));
        }
        found = true;
      }
    }
    const uint32_t col_idx = schema.GetColIdx(col_name);
    return std::make_tuple(col_name,
                           std::make_shared<ColumnValueExpression>(0, col_idx, schema.GetColumn(col_idx)));
  }

  if (children.size() == 2) {
    // The join case. A column reference resolves against exactly one side, and which
    // side it was becomes the expression's tuple index: `#0.k` reads the left input's
    // column k, `#1.k` the right's.
    const auto &left_schema = children[0]->OutputSchema();
    const auto &right_schema = children[1]->OutputSchema();

    auto col_idx_left = left_schema.TryGetColIdx(col_name);
    auto col_idx_right = right_schema.TryGetColIdx(col_name);

    if (col_idx_left.has_value() && col_idx_right.has_value()) {
      throw PlannerException(fmt::format("column {} is ambiguous across the two sides of the join", col_name));
    }
    if (col_idx_left.has_value()) {
      return std::make_tuple(
          col_name, std::make_shared<ColumnValueExpression>(0, *col_idx_left, left_schema.GetColumn(*col_idx_left)));
    }
    if (col_idx_right.has_value()) {
      return std::make_tuple(col_name, std::make_shared<ColumnValueExpression>(
                                           1, *col_idx_right, right_schema.GetColumn(*col_idx_right)));
    }
    throw PlannerException(fmt::format("column {} is not in either side of the join", col_name));
  }

  UNREACHABLE("no plan node evaluates an expression over more than two inputs");
}

auto Planner::PlanConstant(const BoundConstant &expr, const std::vector<AbstractPlanNodeRef> &children)
    -> AbstractExpressionRef {
  return std::make_shared<ConstantValueExpression>(expr.val_);
}

void Planner::AddAggCallToContext(BoundExpression &expr) {
  switch (expr.type_) {
    case ExpressionType::AGG_CALL: {
      // Lift the aggregate call out of the expression and leave a numbered
      // placeholder behind. The real call is planned against the aggregation node's
      // input; the placeholder is later resolved to a reference to the aggregation
      // node's output. That is what makes `max(v1) + max(v2)` work.
      auto &agg_call_expr = dynamic_cast<BoundAggCall &>(expr);
      auto agg_name = fmt::format("__pseudo_agg#{}", ctx_.aggregations_.size());
      auto agg_call =
          BoundAggCall(agg_name, agg_call_expr.is_distinct_, std::vector<std::unique_ptr<BoundExpression>>{});
      ctx_.AddAggregation(std::make_unique<BoundAggCall>(std::exchange(agg_call_expr, std::move(agg_call))));
      return;
    }
    case ExpressionType::COLUMN_REF:
    case ExpressionType::CONSTANT:
      return;
    case ExpressionType::BINARY_OP: {
      auto &binary_op_expr = dynamic_cast<BoundBinaryOp &>(expr);
      AddAggCallToContext(*binary_op_expr.larg_);
      AddAggCallToContext(*binary_op_expr.rarg_);
      return;
    }
    case ExpressionType::UNARY_OP: {
      auto &unary_op_expr = dynamic_cast<BoundUnaryOp &>(expr);
      AddAggCallToContext(*unary_op_expr.arg_);
      return;
    }
    case ExpressionType::FUNC_CALL: {
      auto &func_call_expr = dynamic_cast<BoundFuncCall &>(expr);
      for (const auto &child : func_call_expr.args_) {
        AddAggCallToContext(*child);
      }
      return;
    }
    case ExpressionType::ALIAS: {
      auto &alias_expr = dynamic_cast<BoundAlias &>(expr);
      AddAggCallToContext(*alias_expr.child_);
      return;
    }
    case ExpressionType::TYPE_CAST: {
      auto &cast_expr = dynamic_cast<BoundTypeCast &>(expr);
      AddAggCallToContext(*cast_expr.child_);
      return;
    }
    case ExpressionType::IN_EXPR: {
      auto &in_expr = dynamic_cast<BoundInExpr &>(expr);
      AddAggCallToContext(*in_expr.child_);
      for (const auto &child : in_expr.list_) {
        AddAggCallToContext(*child);
      }
      return;
    }
    case ExpressionType::SUBQUERY:
      // A subquery's aggregates are planned by its own (nested) planner, never lifted here.
      return;
    case ExpressionType::OUTER_COLUMN_REF:
      // Consumed by decorrelation, never by an aggregation of THIS query.
      return;
    default:
      break;
  }
  throw PlannerException(fmt::format("the expression type {} cannot be planned", expr.type_));
}

auto Planner::PlanScalarSubqueryExpression(const BoundSubqueryExpr &subquery_expr) -> AbstractExpressionRef {
  // A correlated scalar that decorrelation already turned into a join reads its value straight
  // from the join output.
  if (auto it = resolved_subqueries_.find(&subquery_expr); it != resolved_subqueries_.end()) {
    return it->second;
  }
  // IN/EXISTS subqueries plan as SEMI/ANTI joins, which only PlanSelect's WHERE handling can
  // build — reaching this point means the subquery sits somewhere a join cannot express.
  if (subquery_expr.kind_ != SubqueryKind::SCALAR) {
    throw NotImplementedException("an IN/EXISTS subquery is only supported as a top-level WHERE conjunct");
  }
  if (subquery_expr.correlated_) {
    throw NotImplementedException("a correlated scalar subquery is only supported inside a WHERE comparison");
  }
  // A fresh planner: the subquery's aggregates/CTEs live in their own scope. The eval hook
  // propagates so a subquery nested inside this one pre-executes the same way.
  Planner sub_planner(catalog_);
  sub_planner.subquery_eval_ = subquery_eval_;
  sub_planner.PlanQuery(*subquery_expr.subquery_);
  auto subplan = sub_planner.plan_;
  const auto &schema = subplan->OutputSchema();
  if (schema.GetColumnCount() != 1) {
    throw PlannerException("scalar subquery must return exactly one column");
  }
  const auto result_type = schema.GetColumn(0).GetType();
  if (subquery_eval_) {
    // Execution path: run the subquery now and substitute its single value as a constant.
    return std::make_shared<ConstantValueExpression>(subquery_eval_(subplan));
  }
  // EXPLAIN path: nothing may run, so keep the planned subquery behind a placeholder.
  return std::make_shared<SubqueryExpression>(subplan, Column::Make("<subquery>", result_type));
}

auto Planner::PlanExpression(const BoundExpression &expr, const std::vector<AbstractPlanNodeRef> &children)
    -> std::tuple<std::string, AbstractExpressionRef> {
  switch (expr.type_) {
    case ExpressionType::AGG_CALL: {
      // By the time we get here the aggregate has already been planned by
      // PlanSelectAgg; what is left is a placeholder pointing at the aggregation
      // node's output column.
      if (ctx_.next_aggregation_ >= ctx_.expr_in_agg_.size()) {
        throw PlannerException("an aggregate call appeared where none was expected");
      }
      return std::make_tuple(UNNAMED_COLUMN, std::move(ctx_.expr_in_agg_[ctx_.next_aggregation_++]));
    }
    case ExpressionType::COLUMN_REF:
      return PlanColumnRef(dynamic_cast<const BoundColumnRef &>(expr), children);
    case ExpressionType::BINARY_OP:
      return std::make_tuple(UNNAMED_COLUMN, PlanBinaryOp(dynamic_cast<const BoundBinaryOp &>(expr), children));
    case ExpressionType::FUNC_CALL:
      return std::make_tuple(UNNAMED_COLUMN, PlanFuncCall(dynamic_cast<const BoundFuncCall &>(expr), children));
    case ExpressionType::CONSTANT:
      return std::make_tuple(UNNAMED_COLUMN, PlanConstant(dynamic_cast<const BoundConstant &>(expr), children));
    case ExpressionType::ALIAS: {
      const auto &alias_expr = dynamic_cast<const BoundAlias &>(expr);
      auto [_, child_expr] = PlanExpression(*alias_expr.child_, children);
      return std::make_tuple(alias_expr.alias_, std::move(child_expr));
    }
    case ExpressionType::TYPE_CAST: {
      const auto &cast_expr = dynamic_cast<const BoundTypeCast &>(expr);
      auto [_, child_expr] = PlanExpression(*cast_expr.child_, children);
      return std::make_tuple(UNNAMED_COLUMN,
                             std::make_shared<CastExpression>(std::move(child_expr), cast_expr.target_,
                                                              /*strict=*/true));
    }
    case ExpressionType::IN_EXPR: {
      const auto &in_expr = dynamic_cast<const BoundInExpr &>(expr);
      std::vector<AbstractExpressionRef> in_children;
      in_children.reserve(in_expr.list_.size() + 1);
      auto [_, tested] = PlanExpression(*in_expr.child_, children);
      in_children.push_back(std::move(tested));
      for (const auto &item : in_expr.list_) {
        auto [_item, item_expr] = PlanExpression(*item, children);
        in_children.push_back(std::move(item_expr));
      }
      return std::make_tuple(UNNAMED_COLUMN,
                             std::make_shared<InExpression>(std::move(in_children), in_expr.negated_));
    }
    case ExpressionType::SUBQUERY:
      return std::make_tuple(UNNAMED_COLUMN,
                             PlanScalarSubqueryExpression(dynamic_cast<const BoundSubqueryExpr &>(expr)));
    case ExpressionType::OUTER_COLUMN_REF:
      // Decorrelation consumes the supported outer refs (an equality in the subquery WHERE);
      // one surviving to this point marks a correlation shape the engine cannot flatten.
      throw NotImplementedException(
          "correlated subqueries support only equality comparisons with outer columns in the WHERE clause");
    case ExpressionType::UNARY_OP: {
      const auto &unary_expr = dynamic_cast<const BoundUnaryOp &>(expr);
      auto [_, arg] = PlanExpression(*unary_expr.arg_, children);
      if (unary_expr.op_name_ == "is_null" || unary_expr.op_name_ == "is_not_null") {
        return std::make_tuple(
            UNNAMED_COLUMN,
            std::make_shared<IsNullExpression>(std::move(arg), unary_expr.op_name_ == "is_not_null"));
      }
      throw PlannerException(fmt::format("the unary operator {} cannot be planned", unary_expr.op_name_));
    }
    default:
      break;
  }
  throw PlannerException(fmt::format("the expression type {} cannot be planned", expr.type_));
}

}  // namespace bumblebee
