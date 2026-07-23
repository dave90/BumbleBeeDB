//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// cast_expression.h
//
// Identification: src/include/execution/expressions/cast_expression.h
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
 * @brief Casts its single child to a target type.
 *
 * Inserted by the planner where a value must be stored at a wider column type than it was produced at —
 * e.g. an INT literal written into a BIGINT column (INSERT VALUES, UPDATE SET). Evaluated with the same
 * `VectorOperations::Cast` the arithmetic/comparison kernels already use, so the child is materialized
 * at the target physical type.
 */
class CastExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct `cast(child as target)`.
   *
   * @param child  The value to cast.
   * @param target The type to cast to (this expression's return type).
   */
  CastExpression(AbstractExpressionRef child, LogicalType target, bool strict = false)
      : AbstractExpression({std::move(child)}, Column::Make("<cast>", target)), strict_(strict) {}

  auto ToString() const -> std::string override {
    return fmt::format("cast({} as {})", GetChildAt(0), GetReturnType().GetType());
  }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(CastExpression);

  /** Explicit SQL CAST: a row that fails to convert raises an error instead of becoming NULL. */
  bool strict_{false};
};

}  // namespace bumblebee
