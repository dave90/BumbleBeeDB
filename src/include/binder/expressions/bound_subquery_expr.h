//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_subquery_expr.h
//
// Identification: src/include/binder/expressions/bound_subquery_expr.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "binder/bound_expression.h"
#include "binder/statement/select_statement.h"

namespace bumblebee {

/** The flavors of subquery expression a SubLink can bind to. */
enum class SubqueryKind : uint8_t {
  /** `(SELECT single_value ...)` used as a scalar. */
  SCALAR,
  /** `[NOT] EXISTS (SELECT ...)`. */
  EXISTS,
  /** `x [NOT] IN (SELECT ...)`, i.e. `= ANY`. */
  ANY,
};

/**
 * A subquery in expression position. Holds the fully bound sub-select; how it is planned
 * depends on the kind (scalar = pre-executed to a constant, ANY/EXISTS = flattened to a join).
 */
class BoundSubqueryExpr : public BoundExpression {
 public:
  /**
   * @brief Construct a bound subquery expression.
   *
   * @param subquery The bound sub-select.
   * @param kind The subquery flavor.
   * @param testexpr The outer-side tested value (ANY only; null otherwise).
   * @param negated True for NOT IN / NOT EXISTS.
   */
  explicit BoundSubqueryExpr(std::unique_ptr<SelectStatement> subquery, SubqueryKind kind,
                             std::unique_ptr<BoundExpression> testexpr, bool negated)
      : BoundExpression(ExpressionType::SUBQUERY),
        subquery_(std::move(subquery)),
        kind_(kind),
        testexpr_(std::move(testexpr)),
        negated_(negated) {}

  auto ToString() const -> std::string override {
    switch (kind_) {
      case SubqueryKind::SCALAR:
        return fmt::format("(SUBQUERY {})", subquery_->ToString());
      case SubqueryKind::EXISTS:
        return fmt::format("({}EXISTS {})", negated_ ? "NOT " : "", subquery_->ToString());
      case SubqueryKind::ANY:
        return fmt::format("({} {}IN {})", testexpr_, negated_ ? "NOT " : "", subquery_->ToString());
    }
    UNREACHABLE("unhandled subquery kind");
  }

  /** The subquery's own aggregates belong to its own scope, never to the enclosing query. */
  auto HasAggregation() const -> bool override { return false; }

  /** The bound sub-select. */
  std::unique_ptr<SelectStatement> subquery_;

  /** The subquery flavor. */
  SubqueryKind kind_;

  /** The outer-side tested value (ANY only; null otherwise). */
  std::unique_ptr<BoundExpression> testexpr_;

  /** True for NOT IN / NOT EXISTS. */
  bool negated_;

  /** True if the subquery references columns of an enclosing query (set by BindSubLink). */
  bool correlated_{false};
};

}  // namespace bumblebee
