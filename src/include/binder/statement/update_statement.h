//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// update_statement.h
//
// Identification: src/include/binder/statement/update_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/bound_statement.h"
#include "binder/expressions/bound_column_ref.h"
#include "binder/table_ref/bound_base_table_ref.h"

namespace bumblebee {

/**
 * A bound UPDATE statement.
 */
class UpdateStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound UPDATE.
   *
   * @param table The target table.
   * @param filter_expr The WHERE clause. A constant `true` when the UPDATE had none.
   * @param target_expr One (column, new value) pair per SET item.
   */
  explicit UpdateStatement(
      std::unique_ptr<BoundBaseTableRef> table, std::unique_ptr<BoundExpression> filter_expr,
      std::vector<std::pair<std::unique_ptr<BoundColumnRef>, std::unique_ptr<BoundExpression>>> target_expr);

  /** The target table. */
  std::unique_ptr<BoundBaseTableRef> table_;

  /** The WHERE clause. */
  std::unique_ptr<BoundExpression> filter_expr_;

  /** One (column, new value) pair per SET item. */
  std::vector<std::pair<std::unique_ptr<BoundColumnRef>, std::unique_ptr<BoundExpression>>> target_expr_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
