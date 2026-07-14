//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_star.h
//
// Identification: src/include/binder/expressions/bound_star.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "binder/bound_expression.h"
#include "common/exception.h"

namespace bumblebee {

/**
 * The `*` in a SELECT list, e.g. `SELECT * FROM x`.
 *
 * The binder expands it into the columns of the scope, so it never reaches the planner.
 */
class BoundStar : public BoundExpression {
 public:
  BoundStar() : BoundExpression(ExpressionType::STAR) {}

  auto HasAggregation() const -> bool override {
    throw Exception("HasAggregation should not have been called on BoundStar");
  }

  auto ToString() const -> std::string override { return "*"; }
};

}  // namespace bumblebee
