//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_binary_op.h
//
// Identification: src/include/binder/expressions/bound_binary_op.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "binder/bound_expression.h"

namespace bumblebee {

/**
 * A bound binary operation, e.g. `a + b`.
 */
class BoundBinaryOp : public BoundExpression {
 public:
  /**
   * @brief Construct a bound binary operation.
   *
   * @param op_name The operator name, e.g. `+`, `=`, `and`.
   * @param larg The left operand.
   * @param rarg The right operand.
   */
  explicit BoundBinaryOp(std::string op_name, std::unique_ptr<BoundExpression> larg,
                         std::unique_ptr<BoundExpression> rarg)
      : BoundExpression(ExpressionType::BINARY_OP),
        op_name_(std::move(op_name)),
        larg_(std::move(larg)),
        rarg_(std::move(rarg)) {}

  auto ToString() const -> std::string override { return fmt::format("({}{}{})", larg_, op_name_, rarg_); }

  auto HasAggregation() const -> bool override { return larg_->HasAggregation() || rarg_->HasAggregation(); }

  /** The operator name. */
  std::string op_name_;

  /** The left operand. */
  std::unique_ptr<BoundExpression> larg_;

  /** The right operand. */
  std::unique_ptr<BoundExpression> rarg_;
};

}  // namespace bumblebee
