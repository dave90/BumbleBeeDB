//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// logic_expression.h
//
// Identification: src/include/execution/expressions/logic_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "fmt/format.h"

namespace bumblebee {

/** The logical connectives. */
enum class LogicType { And, Or };

}  // namespace bumblebee

// Declared before LogicExpression, whose inline ToString() formats a LogicType.
// See the note in comparison_expression.h.
template <>
struct fmt::formatter<bumblebee::LogicType> : fmt::formatter<fmt::string_view> {
  template <typename FormatContext>
  auto format(bumblebee::LogicType c, FormatContext &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::LogicType::And:
        name = "and";
        break;
      case bumblebee::LogicType::Or:
        name = "or";
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
 * Two boolean expressions joined by AND or OR.
 */
class LogicExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `(left <logic_type> right)`.
   *
   * @param left The left operand.
   * @param right The right operand.
   * @param logic_type The connective.
   */
  LogicExpression(AbstractExpressionRef left, AbstractExpressionRef right, LogicType logic_type)
      : AbstractExpression({std::move(left), std::move(right)},
                           Column{"<val>", LogicalType(LogicalTypeId::BOOLEAN)}),
        logic_type_{logic_type} {}

  auto ToString() const -> std::string override {
    return fmt::format("({}{}{})", GetChildAt(0), logic_type_, GetChildAt(1));
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(LogicExpression);

  /**
   * @brief True if `expr` contains an OR anywhere in its tree.
   *
   * The hash-join rule needs this: a conjunction of equalities can become hash-join
   * keys, but a disjunction cannot, so a predicate containing an OR — at any depth —
   * has to stay a nested loop join.
   *
   * @param expr The expression to inspect.
   * @return bool True if an OR is present.
   */
  static auto HasOrPredicate(const AbstractExpressionRef &expr) -> bool {
    if (const auto *logic_expr = dynamic_cast<const LogicExpression *>(expr.get());
        logic_expr != nullptr && logic_expr->logic_type_ == LogicType::Or) {
      return true;
    }
    const auto &children = expr->GetChildren();
    return std::any_of(children.begin(), children.end(),
                       [](const auto &child) { return HasOrPredicate(child); });
  }

  /** The connective. */
  LogicType logic_type_;
};

}  // namespace bumblebee
