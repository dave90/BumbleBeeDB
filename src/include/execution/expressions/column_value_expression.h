//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column_value_expression.h
//
// Identification: src/include/execution/expressions/column_value_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "execution/expressions/abstract_expression.h"

namespace bumblebee {

/**
 * A reference to one column of one input, i.e. `Table.column` reduced to indices.
 */
class ColumnValueExpression : public AbstractExpression {
 public:
  /**
   * @brief Construct a reference to a column of an input.
   *
   * @param tuple_idx Which input: 0 is the only child, or the left side of a join; 1 is the right side.
   * @param col_idx The index of the column within that input's schema.
   * @param ret_type The type of the column.
   */
  ColumnValueExpression(uint32_t tuple_idx, uint32_t col_idx, Column ret_type)
      : AbstractExpression({}, std::move(ret_type)), tuple_idx_{tuple_idx}, col_idx_{col_idx} {}

  /** @return Which input this column comes from: 0 = left, 1 = right. */
  auto GetTupleIdx() const -> uint32_t { return tuple_idx_; }

  /** @return The index of the column within its input's schema. */
  auto GetColIdx() const -> uint32_t { return col_idx_; }

  auto ToString() const -> std::string override { return fmt::format("#{}.{}", tuple_idx_, col_idx_); }

  BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(ColumnValueExpression);

 private:
  /** 0 = the left side of a join (or the sole child), 1 = the right side. */
  uint32_t tuple_idx_;
  /** The column's index within the input schema: schema {A,B,C} has indices {0,1,2}. */
  uint32_t col_idx_;
};

}  // namespace bumblebee
