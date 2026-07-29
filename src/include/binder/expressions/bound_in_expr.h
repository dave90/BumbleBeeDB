//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_in_expr.h
//
// Identification: src/include/binder/expressions/bound_in_expr.h
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
 * A bound `[NOT] IN` over a value list, e.g. `x IN (1, 2, 3)`.
 *
 * Negation is carried as a flag rather than lowered to `NOT(x = a OR ...)` because SQL's
 * three-valued NULL semantics for NOT IN (a NULL in the list poisons every non-match) cannot
 * be reconstructed from an OR-chain once NULL has been folded to false.
 */
class BoundInExpr : public BoundExpression {
 public:
  /**
   * @brief Construct a bound `[NOT] IN` expression.
   *
   * @param child The tested value.
   * @param list The list of candidate values.
   * @param negated True for `NOT IN`.
   */
  explicit BoundInExpr(std::unique_ptr<BoundExpression> child, std::vector<std::unique_ptr<BoundExpression>> list,
                       bool negated)
      : BoundExpression(ExpressionType::IN_EXPR),
        child_(std::move(child)),
        list_(std::move(list)),
        negated_(negated) {}

  auto ToString() const -> std::string override {
    return fmt::format("({} {} ({}))", child_, negated_ ? "NOT IN" : "IN", fmt::join(list_, ", "));
  }

  auto HasAggregation() const -> bool override {
    if (child_->HasAggregation()) {
      return true;
    }
    for (const auto &expr : list_) {
      if (expr->HasAggregation()) {
        return true;
      }
    }
    return false;
  }

  /** The tested value. */
  std::unique_ptr<BoundExpression> child_;

  /** The candidate values. */
  std::vector<std::unique_ptr<BoundExpression>> list_;

  /** True for `NOT IN`. */
  bool negated_;
};

}  // namespace bumblebee
