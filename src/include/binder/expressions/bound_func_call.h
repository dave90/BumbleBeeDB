//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_func_call.h
//
// Identification: src/include/binder/expressions/bound_func_call.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"

namespace bumblebee {

/**
 * A bound scalar function call, e.g. `lower(x)`.
 */
class BoundFuncCall : public BoundExpression {
 public:
  /**
   * @brief Construct a bound function call.
   *
   * @param func_name The function name.
   * @param args The function arguments.
   */
  explicit BoundFuncCall(std::string func_name, std::vector<std::unique_ptr<BoundExpression>> args)
      : BoundExpression(ExpressionType::FUNC_CALL), func_name_(std::move(func_name)), args_(std::move(args)) {}

  auto ToString() const -> std::string override;

  auto HasAggregation() const -> bool override { return false; }

  /** The function name. */
  std::string func_name_;

  /** The function arguments. */
  std::vector<std::unique_ptr<BoundExpression>> args_;
};

}  // namespace bumblebee
