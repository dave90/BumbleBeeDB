//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_alias.h
//
// Identification: src/include/binder/expressions/bound_alias.h
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
 * An alias in a SELECT list, e.g. the `y` in `SELECT count(x) AS y`.
 */
class BoundAlias : public BoundExpression {
 public:
  /**
   * @brief Construct a bound alias.
   *
   * @param alias The alias name.
   * @param child The expression being aliased.
   */
  explicit BoundAlias(std::string alias, std::unique_ptr<BoundExpression> child)
      : BoundExpression(ExpressionType::ALIAS), alias_(std::move(alias)), child_(std::move(child)) {}

  auto ToString() const -> std::string override { return fmt::format("({} as {})", child_, alias_); }

  auto HasAggregation() const -> bool override { return child_->HasAggregation(); }

  /** The alias name. */
  std::string alias_;

  /** The expression being aliased. */
  std::unique_ptr<BoundExpression> child_;
};

}  // namespace bumblebee
