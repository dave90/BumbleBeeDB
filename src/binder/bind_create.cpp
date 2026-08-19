//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bind_create.cpp
//
// Identification: src/binder/bind_create.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// Derived from DuckDB, which is licensed under the MIT License.
// Copyright 2018-2022 Stichting DuckDB Foundation.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_expression.h"
#include "binder/expressions/bound_constant.h"
#include "binder/statement/create_statement.h"
#include "catalog/catalog.h"
#include "catalog/column.h"
#include "common/exception.h"
#include "common/util/string_util.h"
#include "fmt/format.h"
#include "nodes/nodes.hpp"
#include "nodes/parsenodes.hpp"
#include "nodes/pg_list.hpp"
#include "nodes/primnodes.hpp"
#include "nodes/value.hpp"
#include "pg_definitions.hpp"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * @brief Read the single integer typmod of a type name, e.g. the `100` of `VARCHAR(100)`.
 *
 * @param typmods The PGTypeName's typmods list. May be null.
 * @return std::optional<int64_t> The modifier, or nullopt if there was none.
 */
static auto SingleTypmod(duckdb_libpgquery::PGList *typmods) -> std::optional<int64_t> {
  if (typmods == nullptr || typmods->length < 1) {
    return std::nullopt;
  }
  auto *node = reinterpret_cast<duckdb_libpgquery::PGNode *>(typmods->head->data.ptr_value);
  if (node->type != duckdb_libpgquery::T_PGAConst) {
    return std::nullopt;
  }
  const auto &val = reinterpret_cast<duckdb_libpgquery::PGAConst *>(node)->val;
  if (val.type != duckdb_libpgquery::T_PGInteger) {
    return std::nullopt;
  }
  return static_cast<int64_t>(val.val.ival);
}

/**
 * @brief Read the second integer typmod of a type name, e.g. the `2` of `DECIMAL(10, 2)`.
 *
 * @param typmods The PGTypeName's typmods list. May be null.
 * @return std::optional<int64_t> The modifier, or nullopt if there was none.
 */
static auto SecondTypmod(duckdb_libpgquery::PGList *typmods) -> std::optional<int64_t> {
  if (typmods == nullptr || typmods->length < 2) {
    return std::nullopt;
  }
  auto *node = reinterpret_cast<duckdb_libpgquery::PGNode *>(typmods->head->next->data.ptr_value);
  if (node->type != duckdb_libpgquery::T_PGAConst) {
    return std::nullopt;
  }
  const auto &val = reinterpret_cast<duckdb_libpgquery::PGAConst *>(node)->val;
  if (val.type != duckdb_libpgquery::T_PGInteger) {
    return std::nullopt;
  }
  return static_cast<int64_t>(val.val.ival);
}

/**
 * @brief Read the string payload of a `WITH (name = value)` storage option.
 *
 * @param elem The option's PGDefElem.
 * @return The option value rendered as a string.
 */
static auto DefElemString(duckdb_libpgquery::PGDefElem *elem) -> std::string {
  if (elem->arg == nullptr) {
    throw BinderException(fmt::format("storage option '{}' needs a value", elem->defname));
  }
  auto *val = reinterpret_cast<duckdb_libpgquery::PGValue *>(elem->arg);
  switch (val->type) {
    case duckdb_libpgquery::T_PGString:
      return val->val.str;
    case duckdb_libpgquery::T_PGInteger:
      return std::to_string(val->val.ival);
    default:
      throw BinderException(fmt::format("storage option '{}' has an unsupported value type", elem->defname));
  }
}

auto Binder::ResolveTypeName(duckdb_libpgquery::PGTypeName *type_name) -> LogicalType {
  // The base type is the last part of the qualified type name: `pg_catalog.int4` -> `int4`.
  const auto name =
      std::string(reinterpret_cast<duckdb_libpgquery::PGValue *>(type_name->names->tail->data.ptr_value)->val.str);

  auto base_type = LogicalType::FromString(name);
  if (base_type.GetTypeId() == LogicalTypeId::UNKNOWN) {
    throw NotImplementedException(fmt::format("unsupported type: {}", name));
  }

  if (base_type.GetTypeId() == LogicalTypeId::DOUBLE || base_type.GetTypeId() == LogicalTypeId::DECIMAL) {
    // `DECIMAL(w, s)` and `NUMERIC(w, s)` both reach us as `numeric` with two typmods.
    // Without typmods they stay a plain DOUBLE, as bare `DOUBLE` does.
    auto width = SingleTypmod(type_name->typmods);
    auto scale = SecondTypmod(type_name->typmods);
    if (width.has_value() && scale.has_value()) {
      base_type = LogicalType::Decimal(static_cast<int>(*width), static_cast<int>(*scale));
    }
  }
  return base_type;
}

auto Binder::BindColumnDefinition(duckdb_libpgquery::PGColumnDef *cdef) -> Column {
  std::string colname;
  if (cdef->colname != nullptr) {
    colname = cdef->colname;
  }
  if (cdef->collClause != nullptr) {
    throw NotImplementedException("coll clause on column is not supported");
  }

  auto *type_name = cdef->typeName;
  auto base_type = ResolveTypeName(type_name);

  // The declared width of a VARCHAR. Only meaningful for a scalar STRING column;
  // an array of strings stores its payload out of line either way.
  uint32_t varchar_length = VARCHAR_DEFAULT_LENGTH;
  if (base_type.GetTypeId() == LogicalTypeId::STRING) {
    if (auto len = SingleTypmod(type_name->typmods); len.has_value() && *len > 0) {
      varchar_length = static_cast<uint32_t>(*len);
    }
  }

  // A non-null arrayBounds means the column was declared as an array: `INT[]` or `INT[3]`.
  // Postgres gives `INT[]` an arrayBounds list with a single element whose value is -1.
  if (type_name->arrayBounds != nullptr) {
    if (type_name->arrayBounds->length != 1) {
      throw NotImplementedException("only one-dimensional arrays are supported");
    }
    const auto *bound = reinterpret_cast<duckdb_libpgquery::PGValue *>(type_name->arrayBounds->head->data.ptr_value);
    int64_t size = -1;
    if (bound != nullptr && bound->type == duckdb_libpgquery::T_PGInteger) {
      size = static_cast<int64_t>(bound->val.ival);
    }
    auto array_type =
        size >= 0 ? LogicalType::Array(base_type, static_cast<idx_t>(size)) : LogicalType::List(base_type);
    // LIST and ARRAY payloads live outside the row, so the column is variable-length.
    return Column{colname, array_type, 0};
  }

  if (base_type.GetTypeId() == LogicalTypeId::STRING) {
    return Column{colname, base_type, varchar_length};
  }

  return Column::Make(colname, base_type);
}

/** @brief The bound `WITH (...)` storage options of a CREATE TABLE. */
struct BoundStorageOptions {
  StorageFormat format_{StorageFormat::ROW};
  std::string location_;
};

/** @brief Bind the `WITH (format = 'parquet', location = '/path')` options list (may be null).
 * Only those two options exist; anything else is a binder error. */
static auto BindStorageOptions(duckdb_libpgquery::PGList *options) -> BoundStorageOptions {
  BoundStorageOptions out;
  if (options == nullptr) {
    return out;
  }
  for (auto o = options->head; o != nullptr; o = lnext(o)) {
    auto *elem = reinterpret_cast<duckdb_libpgquery::PGDefElem *>(o->data.ptr_value);
    const auto option = StringUtil::Lower(elem->defname);
    if (option == "format") {
      const auto value = StringUtil::Lower(DefElemString(elem));
      if (value != "parquet") {
        throw BinderException(fmt::format("unsupported storage format '{}' (supported: parquet)", value));
      }
      out.format_ = StorageFormat::PARQUET;
    } else if (option == "location") {
      out.location_ = DefElemString(elem);
    } else {
      throw BinderException(fmt::format("unsupported storage option '{}'", option));
    }
  }
  return out;
}

/** @brief Record `key_columns` as THE primary key; a second PRIMARY KEY anywhere in the statement
 * is an error. */
static void SetPrimaryKey(std::vector<std::string> key_columns, std::vector<std::string> &pk) {
  if (!pk.empty()) {
    throw NotImplementedException("cannot have two primary keys");
  }
  pk = std::move(key_columns);
}

/** @brief Bind one column definition's constraint list (only column-level PRIMARY KEY exists). */
static void BindColumnConstraints(duckdb_libpgquery::PGColumnDef *cdef, const std::string &column_name,
                                  std::vector<std::string> &pk) {
  if (cdef->constraints == nullptr) {
    return;
  }
  for (auto constr = cdef->constraints->head; constr != nullptr; constr = constr->next) {
    auto *constraint = reinterpret_cast<duckdb_libpgquery::PGConstraint *>(constr->data.ptr_value);
    if (constraint->contype != duckdb_libpgquery::PG_CONSTR_PRIMARY) {
      throw NotImplementedException("unsupported constraint");
    }
    SetPrimaryKey({column_name}, pk);
  }
}

/** @brief Bind a table-level constraint: `PRIMARY KEY (a, b, ...)`.
 * Deliberately iterates from `cell` over the remaining tableElts cells, as the original code did —
 * table-level constraints come last in the list, and keeping the shape keeps the behaviour. */
static void BindTableConstraints(duckdb_libpgquery::PGListCell *cell, std::vector<std::string> &pk) {
  for (auto con = cell; con != nullptr; con = con->next) {
    auto *constraint = reinterpret_cast<duckdb_libpgquery::PGConstraint *>(con->data.ptr_value);
    if (constraint->contype != duckdb_libpgquery::PG_CONSTR_PRIMARY) {
      throw NotImplementedException("unsupported constraint");
    }
    std::vector<std::string> key_columns;
    for (auto kc = constraint->keys->head; kc != nullptr; kc = kc->next) {
      key_columns.emplace_back(reinterpret_cast<duckdb_libpgquery::PGValue *>(kc->data.ptr_value)->val.str);
    }
    SetPrimaryKey(std::move(key_columns), pk);
  }
}

auto Binder::BindCreate(duckdb_libpgquery::PGCreateStmt *pg_stmt) -> std::unique_ptr<CreateStatement> {
  auto table = std::string(pg_stmt->relation->relname);
  auto columns = std::vector<Column>{};
  std::vector<std::string> pk;

  // Storage options: `WITH (format = 'parquet', location = '/path')` declares an external table.
  auto storage = BindStorageOptions(pg_stmt->options);
  const bool external = storage.format_ == StorageFormat::PARQUET;
  if (external && storage.location_.empty()) {
    throw BinderException("external table needs a location: WITH (format='parquet', location='/path')");
  }
  if (!external && !storage.location_.empty()) {
    throw BinderException("location is only valid together with format='parquet'");
  }

  // An empty column list is only legal for an external table (schema inferred from the files).
  for (auto c = pg_stmt->tableElts != nullptr ? pg_stmt->tableElts->head : nullptr; c != nullptr; c = lnext(c)) {
    auto *node = reinterpret_cast<duckdb_libpgquery::PGNode *>(c->data.ptr_value);
    switch (node->type) {
      case duckdb_libpgquery::T_PGColumnDef: {
        auto *cdef = reinterpret_cast<duckdb_libpgquery::PGColumnDef *>(c->data.ptr_value);
        auto centry = BindColumnDefinition(cdef);
        BindColumnConstraints(cdef, centry.GetName(), pk);
        columns.push_back(std::move(centry));
        break;
      }
      case duckdb_libpgquery::T_PGConstraint:
        BindTableConstraints(c, pk);
        break;
      default:
        throw NotImplementedException("ColumnDef type not handled yet");
    }
  }

  if (columns.empty() && !external) {
    throw BinderException("should have at least 1 column");
  }

  // The `_id` name is reserved for the auto-generated primary key; a user may not declare it (compared
  // case-insensitively, since an unquoted `_ID` folds to `_id`).
  for (const auto &col : columns) {
    if (StringUtil::Lower(col.GetName()) == AUTO_ID_COLUMN) {
      throw BinderException(
          fmt::format("column name '{}' is reserved for the auto-generated primary key", AUTO_ID_COLUMN));
    }
  }

  // External tables live outside the heap/index machinery: no primary key, no auto `_id`.
  if (external) {
    if (!pk.empty()) {
      throw BinderException("external tables do not support PRIMARY KEY");
    }
    auto stmt = std::make_unique<CreateStatement>(std::move(table), std::move(columns), std::vector<std::string>{});
    stmt->format_ = StorageFormat::PARQUET;
    stmt->location_ = std::move(storage.location_);
    return stmt;
  }

  // No PRIMARY KEY declared: prepend an auto-increment `_id` BIGINT column and make it the primary key.
  if (pk.empty()) {
    std::vector<Column> with_id;
    with_id.reserve(columns.size() + 1);
    with_id.push_back(Column::Make(AUTO_ID_COLUMN, LogicalType(LogicalTypeId::BIGINT)));
    for (auto &c : columns) with_id.push_back(std::move(c));

    columns = std::move(with_id);
    pk = {AUTO_ID_COLUMN};
  } else {
    // A declared PRIMARY KEY must name real columns.
    for (const auto &key_col : pk) {
      const bool found =
          std::any_of(columns.begin(), columns.end(), [&](const Column &c) { return c.GetName() == key_col; });
      if (!found) {
        throw BinderException(fmt::format("primary key column '{}' does not exist", key_col));
      }
    }
  }

  return std::make_unique<CreateStatement>(std::move(table), std::move(columns), std::move(pk));
}

}  // namespace bumblebee
