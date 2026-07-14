//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_expression.h
//
// Identification: src/include/execution/expressions/string_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "fmt/format.h"

namespace bumblebee {

/** The supported string functions. */
enum class StringExpressionType { Lower, Upper };

}  // namespace bumblebee

// Declared before StringExpression, whose inline ToString() formats a
// StringExpressionType. See the note in comparison_expression.h.
template <>
struct fmt::formatter<bumblebee::StringExpressionType> : fmt::formatter<fmt::string_view> {
  template <typename FormatContext>
  auto format(bumblebee::StringExpressionType c, FormatContext &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::StringExpressionType::Upper:
        name = "upper";
        break;
      case bumblebee::StringExpressionType::Lower:
        name = "lower";
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
 * A string function applied to one argument.
 */
class StringExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `<expr_type>(arg)`.
   *
   * @param arg The argument. Must be a string.
   * @param expr_type The string function.
   */
  StringExpression(AbstractExpressionRef arg, StringExpressionType expr_type)
      : AbstractExpression({std::move(arg)}, Column::Make("<val>", LogicalType(LogicalTypeId::STRING))),
        expr_type_{expr_type} {
    if (GetChildAt(0)->GetReturnType().GetType() != LogicalType(LogicalTypeId::STRING)) {
      throw NotImplementedException("expected the argument to be a string");
    }
  }

  auto ToString() const -> std::string override { return fmt::format("{}({})", expr_type_, GetChildAt(0)); }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(StringExpression);

  /** The string function. */
  StringExpressionType expr_type_;
};

}  // namespace bumblebee
