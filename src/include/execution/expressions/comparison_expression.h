//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// comparison_expression.h
//
// Identification: src/include/execution/expressions/comparison_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "fmt/format.h"

namespace bumblebee {

/** The comparison operators. */
enum class ComparisonType { Equal, NotEqual, LessThan, LessThanOrEqual, GreaterThan, GreaterThanOrEqual };

}  // namespace bumblebee

// Declared before ComparisonExpression, whose inline ToString() formats a
// ComparisonType: specializing the formatter after that use would be an explicit
// specialization after instantiation, which is ill-formed.
template <>
struct fmt::formatter<bumblebee::ComparisonType> : fmt::formatter<fmt::string_view> {
  template <typename FormatContext>
  auto format(bumblebee::ComparisonType c, FormatContext &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::ComparisonType::Equal:
        name = "=";
        break;
      case bumblebee::ComparisonType::NotEqual:
        name = "!=";
        break;
      case bumblebee::ComparisonType::LessThan:
        name = "<";
        break;
      case bumblebee::ComparisonType::LessThanOrEqual:
        name = "<=";
        break;
      case bumblebee::ComparisonType::GreaterThan:
        name = ">";
        break;
      case bumblebee::ComparisonType::GreaterThanOrEqual:
        name = ">=";
        break;
      default:
        name = "Unknown";
        break;
    }
    return fmt::formatter<fmt::string_view>::format(name, ctx);
  }
};

namespace bumblebee {

/**
 * Two expressions compared against each other. Evaluates to BOOLEAN.
 */
class ComparisonExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `(left <comp_type> right)`.
   *
   * @param left The left operand.
   * @param right The right operand.
   * @param comp_type The comparison operator.
   */
  ComparisonExpression(AbstractExpressionRef left, AbstractExpressionRef right, ComparisonType comp_type)
      : AbstractExpression({std::move(left), std::move(right)},
                           Column{"<val>", LogicalType(LogicalTypeId::BOOLEAN)}),
        comp_type_{comp_type} {}

  auto ToString() const -> std::string override {
    return fmt::format("({}{}{})", GetChildAt(0), comp_type_, GetChildAt(1));
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(ComparisonExpression);

  /** The comparison operator. */
  ComparisonType comp_type_;
};

}  // namespace bumblebee
