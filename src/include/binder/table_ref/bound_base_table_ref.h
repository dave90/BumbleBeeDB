//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_base_table_ref.h
//
// Identification: src/include/binder/table_ref/bound_base_table_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <string>
#include <utility>

#include "binder/bound_table_ref.h"
#include "catalog/schema.h"
#include "common/config.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * A reference to a table in the catalog, e.g. the `y` in `SELECT x FROM y`.
 */
class BoundBaseTableRef : public BoundTableRef {
 public:
  /**
   * @brief Construct a reference to a catalog table.
   *
   * @param table The table name.
   * @param oid The table's oid.
   * @param alias The alias the table was given in the FROM clause, if any.
   * @param schema The table schema.
   */
  explicit BoundBaseTableRef(std::string table, table_oid_t oid, std::optional<std::string> alias, Schema schema)
      : BoundTableRef(TableReferenceType::BASE_TABLE),
        table_(std::move(table)),
        oid_(oid),
        alias_(std::move(alias)),
        schema_(std::move(schema)) {}

  auto ToString() const -> std::string override {
    if (!alias_.has_value()) {
      return fmt::format("BoundBaseTableRef {{ table={}, oid={} }}", table_, oid_);
    }
    return fmt::format("BoundBaseTableRef {{ table={}, oid={}, alias={} }}", table_, oid_, *alias_);
  }

  /** @return The name this table is known by in the current scope: its alias if it has one, else its name. */
  auto GetBoundTableName() const -> std::string {
    if (alias_.has_value()) {
      return *alias_;
    }
    return table_;
  }

  /** The table name. */
  std::string table_;

  /** The table's oid. */
  table_oid_t oid_;

  /** The alias the table was given in the FROM clause, if any. */
  std::optional<std::string> alias_;

  /** The table schema. */
  Schema schema_;
};

}  // namespace bumblebee
