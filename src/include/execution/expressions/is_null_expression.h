//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// is_null_expression.h
//
// Identification: src/include/execution/expressions/is_null_expression.h
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

/**
 * @brief `x IS NULL` / `x IS NOT NULL`: a boolean over the child's validity mask.
 *
 * Unlike a comparison against a NULL literal (which yields NULL and therefore matches nothing),
 * this reads the validity bits directly, so it is the one predicate that can *select* NULL rows.
 */
class IsNullExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `child IS [NOT] NULL`.
   *
   * @param child   The value to test.
   * @param negated True for IS NOT NULL.
   */
  IsNullExpression(AbstractExpressionRef child, bool negated)
      : AbstractExpression({std::move(child)}, Column::Make("<is_null>", LogicalType(LogicalTypeId::BOOLEAN))),
        negated_(negated) {}

  auto ToString() const -> std::string override {
    return fmt::format("({} IS {}NULL)", GetChildAt(0), negated_ ? "NOT " : "");
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(IsNullExpression);

  /** True for IS NOT NULL. */
  bool negated_;
};

}  // namespace bumblebee
