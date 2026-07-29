//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// plan_aggregation.cpp
//
// Identification: src/planner/plan_aggregation.cpp
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
#include "binder/statement/select_statement.h"
#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/cast_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/projection_plan.h"
#include "fmt/format.h"
#include "planner/planner.h"
#include "type/value.h"

namespace bumblebee {

auto Planner::PlanAggCall(const BoundAggCall &agg_call, const std::vector<AbstractPlanNodeRef> &children)
    -> std::tuple<AggregationType, std::vector<AbstractExpressionRef>> {
  if (agg_call.is_distinct_) {
    throw NotImplementedException("DISTINCT inside an aggregate is not supported yet");
  }

  std::vector<AbstractExpressionRef> exprs;
  {
    // An aggregate's argument cannot itself contain an aggregate, so plan it in a
    // scope that forbids one.
    auto guard = NewContext();
    for (const auto &arg : agg_call.args_) {
      auto [_, ret] = PlanExpression(*arg, children);
      // A DECIMAL argument carries its value as a scaled integer; the aggregate kernels accumulate
      // that raw integer in a double and would re-scale it at finalize (SUM/AVG/MIN/MAX come out
      // 10^scale too large). Cast DECIMAL to DOUBLE up front so every aggregate accumulates the
      // real value uniformly (matches the ungrouped path, which already reads values as reals).
      if (ret->GetReturnType().GetType().GetTypeId() == LogicalTypeId::DECIMAL) {
        ret = std::make_shared<CastExpression>(std::move(ret), LogicalType(LogicalTypeId::DOUBLE));
      }
      exprs.emplace_back(std::move(ret));
    }
  }

  return GetAggCallFromFactory(agg_call.func_name_, std::move(exprs));
}

/**
 * @brief Plan a SELECT with aggregation.
 *
 * Aggregation is planned in two passes, because an expression like `max(v1) + max(v2)`
 * straddles the aggregation node: the two `max` calls are computed *by* it, and the
 * `+` is computed *over* its output.
 *
 * Given `SELECT v3, max(v1) + max(v2) FROM t WHERE p GROUP BY v3 HAVING count(v4) > count(v5)`
 * the result is:
 *
 *     Projection  v3, max(v1) + max(v2)
 *       Filter    count(v4) > count(v5)
 *         Aggregation  group_by=[v3] types=[max,max,count,count] exprs=[v1,v2,v4,v5]
 *           <the WHERE / scan plan built by the caller>
 *
 * Pass one walks the select list and the HAVING clause, lifts out every aggregate
 * call into `ctx_.aggregations_`, and leaves a numbered placeholder in its place.
 * Those calls are planned against the aggregation's *input* and become the
 * aggregation node. Pass two plans the HAVING filter and the select-list projection
 * against the aggregation's *output*, where each placeholder resolves to a plain
 * column reference.
 */
auto Planner::PlanSelectAgg(const SelectStatement &statement, AbstractPlanNodeRef child) -> AbstractPlanNodeRef {
  auto guard = NewContext();
  ctx_.allow_aggregation_ = true;

  std::vector<AbstractExpressionRef> group_by_exprs;
  std::vector<std::string> output_col_names;
  for (const auto &expr : statement.group_by_) {
    auto [col_name, abstract_expr] = PlanExpression(*expr, {child});
    group_by_exprs.emplace_back(std::move(abstract_expr));
    output_col_names.emplace_back(std::move(col_name));
  }

  // Pass one: lift every aggregate call out of HAVING and the select list.
  if (!statement.having_->IsInvalid()) {
    AddAggCallToContext(*statement.having_);
  }
  for (auto &item : statement.select_list_) {
    AddAggCallToContext(*item);
  }

  std::vector<AbstractExpressionRef> input_exprs;
  std::vector<AggregationType> agg_types;
  // The aggregation node emits the group-by columns first, then the aggregates.
  const auto agg_begin_idx = group_by_exprs.size();

  size_t term_idx = 0;
  for (const auto &item : ctx_.aggregations_) {
    if (item->type_ != ExpressionType::AGG_CALL) {
      throw NotImplementedException("an alias on an aggregate call is not supported yet");
    }
    const auto &agg_call = dynamic_cast<const BoundAggCall &>(*item);
    auto [agg_type, exprs] = PlanAggCall(agg_call, {child});
    if (exprs.size() > 1) {
      throw NotImplementedException("only aggregates of zero or one argument are supported");
    }
    if (exprs.empty()) {
      // `count(*)` has no argument; count a constant instead.
      input_exprs.emplace_back(std::make_shared<ConstantValueExpression>(Value{static_cast<int32_t>(1)}));
    } else {
      input_exprs.emplace_back(std::move(exprs[0]));
    }

    agg_types.push_back(agg_type);
    output_col_names.emplace_back(fmt::format("agg#{}", term_idx));
    LogicalType agg_result_type =
        AggregationPlanNode::AggResultType(agg_type, input_exprs.back()->GetReturnType().GetType());
    ctx_.expr_in_agg_.emplace_back(std::make_shared<ColumnValueExpression>(
        0, agg_begin_idx + term_idx, Column::Make("<agg_result>", std::move(agg_result_type))));

    term_idx++;
  }

  auto agg_output_schema = AggregationPlanNode::InferAggSchema(group_by_exprs, input_exprs, agg_types);

  AbstractPlanNodeRef plan = std::make_shared<AggregationPlanNode>(
      std::make_shared<Schema>(ProjectionPlanNode::RenameSchema(agg_output_schema, output_col_names)),
      std::move(child), std::move(group_by_exprs), std::move(input_exprs), std::move(agg_types));

  // Pass two: everything above the aggregation node.
  if (!statement.having_->IsInvalid()) {
    auto [_, expr] = PlanExpression(*statement.having_, {plan});
    plan = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(plan->OutputSchema()), std::move(expr),
                                            std::move(plan));
  }

  std::vector<AbstractExpressionRef> exprs;
  std::vector<std::string> final_output_col_names;
  for (const auto &item : statement.select_list_) {
    auto [name, expr] = PlanExpression(*item, {plan});
    if (name == UNNAMED_COLUMN) {
      name = fmt::format("__unnamed#{}", universal_id_++);
    }
    exprs.push_back(std::move(expr));
    final_output_col_names.emplace_back(std::move(name));
  }

  return std::make_shared<ProjectionPlanNode>(
      std::make_shared<Schema>(ProjectionPlanNode::RenameSchema(
          ProjectionPlanNode::InferProjectionSchema(exprs), final_output_col_names)),
      std::move(exprs), std::move(plan));
}

}  // namespace bumblebee
