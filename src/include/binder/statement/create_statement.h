//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// create_statement.h
//
// Identification: src/include/binder/statement/create_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "binder/bound_statement.h"
#include "catalog/column.h"
#include "storage/table/table_storage.h"

namespace bumblebee {

/**
 * A bound CREATE TABLE statement.
 */
class CreateStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound CREATE TABLE.
   *
   * @param table The table name.
   * @param columns The column definitions.
   * @param primary_key The names of the primary key columns, if any.
   */
  explicit CreateStatement(std::string table, std::vector<Column> columns, std::vector<std::string> primary_key);

  /** The table name. */
  std::string table_;

  /** The column definitions. Empty only for an external table whose schema is inferred from the
   * files at `location_`. */
  std::vector<Column> columns_;

  /** The names of the primary key columns, if any. */
  std::vector<std::string> primary_key_;

  /** The storage backend: ROW (default) or PARQUET for `WITH (format='parquet', location=...)`. */
  StorageFormat format_{StorageFormat::ROW};

  /** The folder holding the external table's data files (PARQUET only). */
  std::string location_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
