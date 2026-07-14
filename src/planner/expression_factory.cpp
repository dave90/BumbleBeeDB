//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// expression_factory.cpp
//
// Identification: src/planner/expression_factory.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "binder/expressions/bound_func_call.h"
#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/arithmetic_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/expressions/string_expression.h"
#include "execution/plans/aggregation_plan.h"
#include "fmt/format.h"
#include "planner/planner.h"

namespace bumblebee {

auto Planner::GetAggCallFromFactory(const std::string &func_name, std::vector<AbstractExpressionRef> args)
    -> std::tuple<AggregationType, std::vector<AbstractExpressionRef>> {
  if (args.empty() && func_name == "count_star") {
    return {AggregationType::CountStarAggregate, {}};
  }
  if (args.size() == 1) {
    auto expr = std::move(args[0]);
    if (func_name == "min") {
      return {AggregationType::MinAggregate, {std::move(expr)}};
    }
    if (func_name == "max") {
      return {AggregationType::MaxAggregate, {std::move(expr)}};
    }
    if (func_name == "sum") {
      return {AggregationType::SumAggregate, {std::move(expr)}};
    }
    if (func_name == "count") {
      return {AggregationType::CountAggregate, {std::move(expr)}};
    }
  }
  throw PlannerException(fmt::format("no aggregate {} taking {} argument(s)", func_name, args.size()));
}

auto Planner::GetBinaryExpressionFromFactory(const std::string &op_name, AbstractExpressionRef left,
                                             AbstractExpressionRef right) -> AbstractExpressionRef {
  if (op_name == "=" || op_name == "==") {
    return std::make_shared<ComparisonExpression>(std::move(left), std::move(right), ComparisonType::Equal);
  }
  if (op_name == "!=" || op_name == "<>") {
    return std::make_shared<ComparisonExpression>(std::move(left), std::move(right), ComparisonType::NotEqual);
  }
  if (op_name == "<") {
    return std::make_shared<ComparisonExpression>(std::move(left), std::move(right), ComparisonType::LessThan);
  }
  if (op_name == "<=") {
    return std::make_shared<ComparisonExpression>(std::move(left), std::move(right),
                                                  ComparisonType::LessThanOrEqual);
  }
  if (op_name == ">") {
    return std::make_shared<ComparisonExpression>(std::move(left), std::move(right), ComparisonType::GreaterThan);
  }
  if (op_name == ">=") {
    return std::make_shared<ComparisonExpression>(std::move(left), std::move(right),
                                                  ComparisonType::GreaterThanOrEqual);
  }
  if (op_name == "+") {
    return std::make_shared<ArithmeticExpression>(std::move(left), std::move(right), ArithmeticType::Plus);
  }
  if (op_name == "-") {
    return std::make_shared<ArithmeticExpression>(std::move(left), std::move(right), ArithmeticType::Minus);
  }
  if (op_name == "*") {
    return std::make_shared<ArithmeticExpression>(std::move(left), std::move(right), ArithmeticType::Multiply);
  }
  if (op_name == "/") {
    return std::make_shared<ArithmeticExpression>(std::move(left), std::move(right), ArithmeticType::Divide);
  }
  if (op_name == "and") {
    return std::make_shared<LogicExpression>(std::move(left), std::move(right), LogicType::And);
  }
  if (op_name == "or") {
    return std::make_shared<LogicExpression>(std::move(left), std::move(right), LogicType::Or);
  }
  throw PlannerException(fmt::format("the binary operator {} cannot be planned", op_name));
}

auto Planner::GetFuncCallFromFactory(const std::string &func_name, std::vector<AbstractExpressionRef> args)
    -> AbstractExpressionRef {
  if (func_name == "lower" || func_name == "upper") {
    if (args.size() != 1) {
      throw PlannerException(fmt::format("{} takes exactly one argument, got {}", func_name, args.size()));
    }
    const auto type = func_name == "lower" ? StringExpressionType::Lower : StringExpressionType::Upper;
    return std::make_shared<StringExpression>(std::move(args[0]), type);
  }
  throw PlannerException(fmt::format("the function {} cannot be planned", func_name));
}

auto Planner::PlanFuncCall(const BoundFuncCall &expr, const std::vector<AbstractPlanNodeRef> &children)
    -> AbstractExpressionRef {
  std::vector<AbstractExpressionRef> args;
  args.reserve(expr.args_.size());
  for (const auto &arg : expr.args_) {
    auto [_, arg_expr] = PlanExpression(*arg, children);
    args.push_back(std::move(arg_expr));
  }
  return GetFuncCallFromFactory(expr.func_name_, std::move(args));
}

}  // namespace bumblebee
