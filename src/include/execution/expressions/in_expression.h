//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// in_expression.h
//
// Identification: src/include/execution/expressions/in_expression.h
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

/**
 * A SQL `[NOT] IN` predicate over a value list: `x IN (a, b, c)`, evaluating to BOOLEAN.
 *
 * Child 0 is the tested value; children 1..N are the list. Negation lives inside the node
 * (not as an outer NOT) because NOT IN must see NULLs: a row whose value matches nothing but
 * whose comparison involved a NULL (the tested value or a list element) yields NULL, which
 * this engine's boolean convention folds to 0 — so NOT IN emits 1 only when there was no
 * match AND no NULL was involved.
 */
class InExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `child [NOT] IN (list...)`.
   *
   * @param children The tested value followed by the list values (at least 2 entries).
   * @param negated True for `NOT IN`.
   */
  explicit InExpression(std::vector<AbstractExpressionRef> children, bool negated)
      : AbstractExpression(std::move(children), Column{"<val>", LogicalType(LogicalTypeId::BOOLEAN)}),
        negated_{negated} {}

  auto ToString() const -> std::string override {
    std::string list;
    for (size_t i = 1; i < children_.size(); i++) {
      if (i > 1) {
        list += ", ";
      }
      list += children_[i]->ToString();
    }
    return fmt::format("({} {} ({}))", GetChildAt(0), negated_ ? "NOT IN" : "IN", list);
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(InExpression);

  /** True for `NOT IN`. */
  bool negated_;
};

}  // namespace bumblebee
