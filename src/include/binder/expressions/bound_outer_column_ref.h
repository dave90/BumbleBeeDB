//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_outer_column_ref.h
//
// Identification: src/include/binder/expressions/bound_outer_column_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "binder/expressions/bound_column_ref.h"

namespace bumblebee {

/**
 * A column of an ENCLOSING query referenced inside a correlated subquery, e.g. the `o_orderkey`
 * in `EXISTS (SELECT 1 FROM lineitem WHERE l_orderkey = o_orderkey)`.
 *
 * The planner never evaluates one of these directly: decorrelation consumes them (an equality
 * against an outer ref becomes a join key on the enclosing plan), and any that survive to
 * PlanExpression mark a correlation shape the engine does not support.
 */
class BoundOuterColumnRef : public BoundExpression {
 public:
  /**
   * @brief Construct an outer column reference.
   *
   * @param inner The column reference, resolved against the enclosing scope.
   * @param depth How many query levels up it resolved (1 = the immediately enclosing query).
   */
  explicit BoundOuterColumnRef(std::unique_ptr<BoundColumnRef> inner, size_t depth)
      : BoundExpression(ExpressionType::OUTER_COLUMN_REF), inner_(std::move(inner)), depth_(depth) {}

  auto ToString() const -> std::string override { return fmt::format("OUTER#{}({})", depth_, inner_->ToString()); }

  auto HasAggregation() const -> bool override { return false; }

  /** The column reference, resolved against the enclosing scope. */
  std::unique_ptr<BoundColumnRef> inner_;

  /** How many query levels up it resolved (1 = the immediately enclosing query). */
  size_t depth_;
};

}  // namespace bumblebee
