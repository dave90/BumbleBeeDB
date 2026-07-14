//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_constant.h
//
// Identification: src/include/binder/expressions/bound_constant.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>

#include "binder/bound_expression.h"
#include "type/value.h"

namespace bumblebee {

/**
 * A bound constant, e.g. `1`.
 */
class BoundConstant : public BoundExpression {
 public:
  /**
   * @brief Construct a bound constant.
   *
   * @param val The literal value.
   */
  explicit BoundConstant(Value val) : BoundExpression(ExpressionType::CONSTANT), val_(std::move(val)) {}

  auto ToString() const -> std::string override { return val_.ToString(); }

  auto HasAggregation() const -> bool override { return false; }

  /** The constant being bound. */
  Value val_;
};

}  // namespace bumblebee
