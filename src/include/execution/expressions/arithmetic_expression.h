//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// arithmetic_expression.h
//
// Identification: src/include/execution/expressions/arithmetic_expression.h
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
#include "type/logical_type.h"

namespace bumblebee {

/** The arithmetic operators. */
enum class ArithmeticType { Plus, Minus, Multiply, Divide };

}  // namespace bumblebee

// Declared before ArithmeticExpression, whose inline ToString() formats an
// ArithmeticType. See the note in comparison_expression.h.
template <>
struct fmt::formatter<bumblebee::ArithmeticType> : fmt::formatter<fmt::string_view> {
  template <typename FormatContext>
  auto format(bumblebee::ArithmeticType c, FormatContext &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::ArithmeticType::Plus:
        name = "+";
        break;
      case bumblebee::ArithmeticType::Minus:
        name = "-";
        break;
      case bumblebee::ArithmeticType::Multiply:
        name = "*";
        break;
      case bumblebee::ArithmeticType::Divide:
        name = "/";
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
 * Two expressions combined arithmetically.
 */
class ArithmeticExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `(left <compute_type> right)`.
   *
   * The result type is the common type of the two operands, so `INT + BIGINT` is a
   * BIGINT.
   *
   * @param left The left operand.
   * @param right The right operand.
   * @param compute_type The arithmetic operator.
   */
  ArithmeticExpression(AbstractExpressionRef left, AbstractExpressionRef right, ArithmeticType compute_type)
      : AbstractExpression({left, right}, Column::Make("<val>", ResultType(left, right))),
        compute_type_{compute_type} {}

  auto ToString() const -> std::string override {
    return fmt::format("({}{}{})", GetChildAt(0), compute_type_, GetChildAt(1));
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(ArithmeticExpression);

  /** The arithmetic operator. */
  ArithmeticType compute_type_;

 private:
  /**
   * @brief The result type of `left <op> right`: the common type, except DECIMAL is promoted to
   * DOUBLE.
   *
   * DECIMAL `*` / `/` need scale-aware algebra (the raw-integer product carries scale `s1+s2`, etc.)
   * that the arithmetic dispatch does not apply on the equal-type fast path, so a DECIMAL result
   * would come out 10^scale off. Promoting to DOUBLE runs the operation on the real values via the
   * correct decimal->double unscaling path — inexact past 2^53, consistent with how aggregates
   * already handle DECIMAL.
   */
  static auto ResultType(const AbstractExpressionRef &left, const AbstractExpressionRef &right) -> LogicalType {
    auto common =
        LogicalType::CommonType(left->GetReturnType().GetType(), right->GetReturnType().GetType());
    if (common.GetTypeId() == LogicalTypeId::DECIMAL) {
      return LogicalType(LogicalTypeId::DOUBLE);
    }
    return common;
  }
};

}  // namespace bumblebee
