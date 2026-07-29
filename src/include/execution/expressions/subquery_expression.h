//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// subquery_expression.h
//
// Identification: src/include/execution/expressions/subquery_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "execution/expressions/abstract_expression.h"

namespace bumblebee {

class AbstractPlanNode;

/**
 * A scalar subquery that has been planned but NOT executed. This placeholder exists only on the
 * EXPLAIN path (where nothing may run): during normal execution the planner pre-executes the
 * subplan through `Planner::subquery_eval_` and emits a ConstantValueExpression instead, so this
 * node must never reach the expression executor.
 */
class SubqueryExpression : public AbstractExpression {
 public:
  /**
   * @brief Wrap a planned scalar subquery.
   *
   * @param subplan The subquery's logical plan (one output column).
   * @param ret_type The subquery's output column type.
   */
  SubqueryExpression(std::shared_ptr<const AbstractPlanNode> subplan, Column ret_type)
      : AbstractExpression({}, std::move(ret_type)), subplan_(std::move(subplan)) {}

  auto ToString() const -> std::string override { return "(subquery)"; }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(SubqueryExpression);

  /** The subquery's logical plan. */
  std::shared_ptr<const AbstractPlanNode> subplan_;
};

}  // namespace bumblebee
