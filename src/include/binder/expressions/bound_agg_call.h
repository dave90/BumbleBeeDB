//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_agg_call.h
//
// Identification: src/include/binder/expressions/bound_agg_call.h
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
 * A bound aggregate call, e.g. `sum(x)`.
 */
class BoundAggCall : public BoundExpression {
 public:
  /**
   * @brief Construct a bound aggregate call.
   *
   * @param func_name The aggregate name, e.g. `sum`, `count_star`.
   * @param is_distinct True for `agg(DISTINCT ...)`.
   * @param args The aggregate arguments.
   */
  explicit BoundAggCall(std::string func_name, bool is_distinct, std::vector<std::unique_ptr<BoundExpression>> args)
      : BoundExpression(ExpressionType::AGG_CALL),
        func_name_(std::move(func_name)),
        is_distinct_(is_distinct),
        args_(std::move(args)) {}

  auto ToString() const -> std::string override;

  auto HasAggregation() const -> bool override { return true; }

  /** The aggregate name. */
  std::string func_name_;

  /** True for `agg(DISTINCT ...)`. */
  bool is_distinct_;

  /** The aggregate arguments. */
  std::vector<std::unique_ptr<BoundExpression>> args_;
};

}  // namespace bumblebee
