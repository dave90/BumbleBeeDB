//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// insert_statement.h
//
// Identification: src/include/binder/statement/insert_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "binder/bound_statement.h"
#include "binder/table_ref/bound_base_table_ref.h"

namespace bumblebee {

class SelectStatement;

/**
 * A bound INSERT statement.
 *
 * Both `INSERT INTO t VALUES (...)` and `INSERT INTO t SELECT ...` bind to a
 * target table plus a SELECT producing the rows; a VALUES clause is just a
 * SELECT over a BoundExpressionListRef.
 */
class InsertStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound INSERT.
   *
   * @param table The target table.
   * @param select The SELECT producing the rows to insert.
   */
  explicit InsertStatement(std::unique_ptr<BoundBaseTableRef> table, std::unique_ptr<SelectStatement> select);

  /** The target table. */
  std::unique_ptr<BoundBaseTableRef> table_;

  /** The SELECT producing the rows to insert. */
  std::unique_ptr<SelectStatement> select_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
