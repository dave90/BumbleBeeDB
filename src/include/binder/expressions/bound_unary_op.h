//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_unary_op.h
//
// Identification: src/include/binder/expressions/bound_unary_op.h
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
 * A bound unary operation, e.g. `-x`.
 */
class BoundUnaryOp : public BoundExpression {
 public:
  /**
   * @brief Construct a bound unary operation.
   *
   * @param op_name The operator name, e.g. `-`, `not`.
   * @param arg The operand.
   */
  explicit BoundUnaryOp(std::string op_name, std::unique_ptr<BoundExpression> arg)
      : BoundExpression(ExpressionType::UNARY_OP), op_name_(std::move(op_name)), arg_(std::move(arg)) {}

  auto ToString() const -> std::string override { return fmt::format("({}{})", op_name_, arg_); }

  auto HasAggregation() const -> bool override { return arg_->HasAggregation(); }

  /** The operator name. */
  std::string op_name_;

  /** The operand. */
  std::unique_ptr<BoundExpression> arg_;
};

}  // namespace bumblebee
