//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// like_expression.h
//
// Identification: src/include/execution/expressions/like_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>

#include "execution/expressions/abstract_expression.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * A SQL `LIKE` / `NOT LIKE` predicate: `input LIKE pattern`, evaluating to BOOLEAN.
 *
 * The pattern uses `%` (any run of characters, including none) and `_` (any single character);
 * all other bytes match literally. Matching is byte-wise, so `_` counts a UTF-8 byte rather than a
 * codepoint (a known limitation; `%` substring/anchor matching is unaffected). There is no ESCAPE
 * support yet.
 */
class LikeExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `input [NOT] LIKE pattern`.
   *
   * @param input The string being tested.
   * @param pattern The LIKE pattern.
   * @param negated True for `NOT LIKE`.
   */
  LikeExpression(AbstractExpressionRef input, AbstractExpressionRef pattern, bool negated)
      : AbstractExpression({std::move(input), std::move(pattern)},
                           Column{"<val>", LogicalType(LogicalTypeId::BOOLEAN)}),
        negated_{negated} {}

  auto ToString() const -> std::string override {
    return fmt::format("({} {} {})", GetChildAt(0), negated_ ? "NOT LIKE" : "LIKE", GetChildAt(1));
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(LikeExpression);

  /** True for `NOT LIKE`. */
  bool negated_;
};

}  // namespace bumblebee
