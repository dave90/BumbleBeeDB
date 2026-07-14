//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// delete_statement.h
//
// Identification: src/include/binder/statement/delete_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "binder/bound_expression.h"
#include "binder/bound_statement.h"
#include "binder/table_ref/bound_base_table_ref.h"

namespace bumblebee {

/**
 * A bound DELETE statement.
 */
class DeleteStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound DELETE.
   *
   * @param table The target table.
   * @param expr The WHERE clause. A constant `true` when the DELETE had none.
   */
  explicit DeleteStatement(std::unique_ptr<BoundBaseTableRef> table, std::unique_ptr<BoundExpression> expr);

  /** The target table. */
  std::unique_ptr<BoundBaseTableRef> table_;

  /** The WHERE clause. */
  std::unique_ptr<BoundExpression> expr_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
