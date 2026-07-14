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

namespace {

/**
 * @brief Read the single integer typmod of a type name, e.g. the `100` of `VARCHAR(100)`.
 *
 * @param typmods The PGTypeName's typmods list. May be null.
 * @return std::optional<int64_t> The modifier, or nullopt if there was none.
 */
auto SingleTypmod(duckdb_libpgquery::PGList *typmods) -> std::optional<int64_t> {
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
auto SecondTypmod(duckdb_libpgquery::PGList *typmods) -> std::optional<int64_t> {
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

}  // namespace

auto Binder::BindColumnDefinition(duckdb_libpgquery::PGColumnDef *cdef) -> Column {
  std::string colname;
  if (cdef->colname != nullptr) {
    colname = cdef->colname;
  }
  if (cdef->collClause != nullptr) {
    throw NotImplementedException("coll clause on column is not supported");
  }

  auto *type_name = cdef->typeName;
  // The base type is the last part of the qualified type name: `pg_catalog.int4` -> `int4`.
  const auto name = std::string(
      reinterpret_cast<duckdb_libpgquery::PGValue *>(type_name->names->tail->data.ptr_value)->val.str);

  auto base_type = LogicalType::FromString(name);
  if (base_type.GetTypeId() == LogicalTypeId::UNKNOWN) {
    throw NotImplementedException(fmt::format("unsupported type: {}", name));
  }

  // The declared width of a VARCHAR. Only meaningful for a scalar STRING column;
  // an array of strings stores its payload out of line either way.
  uint32_t varchar_length = VARCHAR_DEFAULT_LENGTH;

  if (base_type.GetTypeId() == LogicalTypeId::STRING) {
    if (auto len = SingleTypmod(type_name->typmods); len.has_value() && *len > 0) {
      varchar_length = static_cast<uint32_t>(*len);
    }
  } else if (base_type.GetTypeId() == LogicalTypeId::DOUBLE || base_type.GetTypeId() == LogicalTypeId::DECIMAL) {
    // `DECIMAL(w, s)` and `NUMERIC(w, s)` both reach us as `numeric` with two typmods.
    // Without typmods they stay a plain DOUBLE, as bare `DOUBLE` does.
    auto width = SingleTypmod(type_name->typmods);
    auto scale = SecondTypmod(type_name->typmods);
    if (width.has_value() && scale.has_value()) {
      base_type = LogicalType::Decimal(static_cast<int>(*width), static_cast<int>(*scale));
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
    auto array_type = size >= 0 ? LogicalType::Array(base_type, static_cast<idx_t>(size))
                                : LogicalType::List(base_type);
    // LIST and ARRAY payloads live outside the row, so the column is variable-length.
    return Column{colname, array_type, 0};
  }

  if (base_type.GetTypeId() == LogicalTypeId::STRING) {
    return Column{colname, base_type, varchar_length};
  }

  return Column::Make(colname, base_type);
}

auto Binder::BindCreate(duckdb_libpgquery::PGCreateStmt *pg_stmt) -> std::unique_ptr<CreateStatement> {
  auto table = std::string(pg_stmt->relation->relname);
  auto columns = std::vector<Column>{};
  std::vector<std::string> pk;

  for (auto c = pg_stmt->tableElts->head; c != nullptr; c = lnext(c)) {
    auto *node = reinterpret_cast<duckdb_libpgquery::PGNode *>(c->data.ptr_value);
    switch (node->type) {
      case duckdb_libpgquery::T_PGColumnDef: {
        auto *cdef = reinterpret_cast<duckdb_libpgquery::PGColumnDef *>(c->data.ptr_value);
        auto centry = BindColumnDefinition(cdef);
        if (cdef->constraints != nullptr) {
          for (auto constr = cdef->constraints->head; constr != nullptr; constr = constr->next) {
            auto *constraint = reinterpret_cast<duckdb_libpgquery::PGConstraint *>(constr->data.ptr_value);
            switch (constraint->contype) {
              case duckdb_libpgquery::PG_CONSTR_PRIMARY: {
                if (!pk.empty()) {
                  throw NotImplementedException("cannot have two primary keys");
                }
                pk = {centry.GetName()};
                break;
              }
              default:
                throw NotImplementedException("unsupported constraint");
            }
          }
        }
        columns.push_back(std::move(centry));
        break;
      }
      case duckdb_libpgquery::T_PGConstraint: {
        for (auto con = c; con != nullptr; con = con->next) {
          auto *constraint = reinterpret_cast<duckdb_libpgquery::PGConstraint *>(con->data.ptr_value);
          switch (constraint->contype) {
            case duckdb_libpgquery::PG_CONSTR_PRIMARY: {
              std::vector<std::string> key_columns;
              for (auto kc = constraint->keys->head; kc != nullptr; kc = kc->next) {
                key_columns.emplace_back(reinterpret_cast<duckdb_libpgquery::PGValue *>(kc->data.ptr_value)->val.str);
              }
              if (!pk.empty()) {
                throw NotImplementedException("cannot have two primary keys");
              }
              pk = std::move(key_columns);
              break;
            }
            default:
              throw NotImplementedException("unsupported constraint");
          }
        }
        break;
      }
      default:
        throw NotImplementedException("ColumnDef type not handled yet");
    }
  }

  if (columns.empty()) {
    throw BinderException("should have at least 1 column");
  }

  return std::make_unique<CreateStatement>(std::move(table), std::move(columns), std::move(pk));
}

}  // namespace bumblebee
