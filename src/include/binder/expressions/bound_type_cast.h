//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_type_cast.h
//
// Identification: src/include/binder/expressions/bound_type_cast.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "binder/bound_expression.h"
#include "fmt/format.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * A bound explicit cast: `CAST(child AS type)` / `child::type`.
 *
 * Explicit casts are strict — a value that fails to convert raises an error at execution, unlike
 * the planner's implicit storage-width coercions (which the same kernels run in NULL-on-failure
 * mode).
 */
class BoundTypeCast : public BoundExpression {
 public:
  BoundTypeCast(std::unique_ptr<BoundExpression> child, LogicalType target)
      : BoundExpression(ExpressionType::TYPE_CAST), child_(std::move(child)), target_(std::move(target)) {}

  auto ToString() const -> std::string override {
    return fmt::format("cast({} as {})", child_->ToString(), target_.ToString());
  }

  auto HasAggregation() const -> bool override { return child_->HasAggregation(); }

  /** The value being cast. */
  std::unique_ptr<BoundExpression> child_;

  /** The target type. */
  LogicalType target_;
};

}  // namespace bumblebee
