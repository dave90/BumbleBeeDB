//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// constant_value_expression.h
//
// Identification: src/include/execution/expressions/constant_value_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "type/value.h"

namespace bumblebee {

/**
 * A literal.
 */
class ConstantValueExpression : public AbstractExpression {
 public:
  /**
   * @brief Wrap a value as an expression.
   *
   * @param val The value.
   */
  explicit ConstantValueExpression(const Value &val)
      : AbstractExpression({}, Column::Make("<val>", val.GetType())), val_(val) {}

  auto ToString() const -> std::string override { return val_.ToString(); }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(ConstantValueExpression);

  /** The literal. */
  Value val_;
};

}  // namespace bumblebee
