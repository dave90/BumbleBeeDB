//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_column_ref.h
//
// Identification: src/include/binder/expressions/bound_column_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "common/macros.h"
#include "fmt/ranges.h"

namespace bumblebee {

/**
 * A bound column reference, e.g. the `y.x` in a SELECT list.
 *
 * The name is kept as its dotted parts, so `y.x` is `{"y", "x"}`.
 */
class BoundColumnRef : public BoundExpression {
 public:
  /**
   * @brief Construct a bound column reference.
   *
   * @param col_name The dotted parts of the column name.
   */
  explicit BoundColumnRef(std::vector<std::string> col_name)
      : BoundExpression(ExpressionType::COLUMN_REF), col_name_(std::move(col_name)) {}

  /**
   * @brief Qualify a column reference with one more leading name part.
   *
   * @param self The column reference. May be null, in which case null is returned.
   * @param prefix The name part to prepend.
   * @return std::unique_ptr<BoundColumnRef> The qualified column reference.
   */
  static auto Prepend(std::unique_ptr<BoundColumnRef> self, std::string prefix) -> std::unique_ptr<BoundColumnRef> {
    if (self == nullptr) {
      return nullptr;
    }
    std::vector<std::string> col_name{std::move(prefix)};
    std::copy(self->col_name_.cbegin(), self->col_name_.cend(), std::back_inserter(col_name));
    return std::make_unique<BoundColumnRef>(std::move(col_name));
  }

  auto ToString() const -> std::string override { return fmt::format("{}", fmt::join(col_name_, ".")); }

  auto HasAggregation() const -> bool override { return false; }

  /** The dotted parts of the column name. */
  std::vector<std::string> col_name_;
};

}  // namespace bumblebee
