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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/bound_order_by.h"
#include "binder/bound_table_ref.h"
#include "binder/expressions/bound_constant.h"
#include "binder/statement/select_statement.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/values_plan.h"
#include "fmt/format.h"
#include "planner/planner.h"
#include "type/value.h"

namespace bumblebee {

namespace {

/**
 * @brief Read a LIMIT / OFFSET clause, which must be an integer literal.
 *
 * @param expr The bound clause.
 * @param clause_name "LIMIT" or "OFFSET", for the error message.
 * @return std::optional<size_t> The value, or nullopt if the clause is absent.
 */
auto PlanLimitValue(const BoundExpression &expr, const char *clause_name) -> std::optional<size_t> {
  if (expr.IsInvalid()) {
    return std::nullopt;
  }
  if (expr.type_ == ExpressionType::CONSTANT) {
    const auto &constant_expr = dynamic_cast<const BoundConstant &>(expr);
    if (!constant_expr.val_.IsNull() &&
        constant_expr.val_.GetType() == LogicalType(LogicalTypeId::INTEGER)) {
      return std::make_optional(static_cast<size_t>(constant_expr.val_.GetAs<int32_t>()));
    }
  }
  throw NotImplementedException(fmt::format("the {} clause must be an integer constant", clause_name));
}

}  // namespace

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
    auto schema = plan->OutputSchema();
    auto [_, expr] = PlanExpression(*statement.where_, {plan});
    plan = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(schema), std::move(expr), std::move(plan));
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
    plan = std::make_shared<ProjectionPlanNode>(
        std::make_shared<Schema>(
            ProjectionPlanNode::RenameSchema(ProjectionPlanNode::InferProjectionSchema(exprs), column_names)),
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
                                                 std::move(distinct_exprs),
                                                 std::vector<AbstractExpressionRef>{},
                                                 std::vector<AggregationType>{});
  }

  if (!statement.sort_.empty()) {
    std::vector<OrderBy> order_bys;
    order_bys.reserve(statement.sort_.size());
    for (const auto &order_by : statement.sort_) {
      auto [_, expr] = PlanExpression(*order_by->expr_, {plan});
      order_bys.emplace_back(order_by->type_, order_by->null_order_, std::move(expr));
    }
    plan = std::make_shared<SortPlanNode>(std::make_shared<Schema>(plan->OutputSchema()), plan,
                                          std::move(order_bys));
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

}  // namespace bumblebee
