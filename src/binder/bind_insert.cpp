//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bind_insert.cpp
//
// Identification: src/binder/bind_insert.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_expression.h"
#include "binder/expressions/bound_column_ref.h"
#include "binder/expressions/bound_constant.h"
#include "binder/statement/delete_statement.h"
#include "binder/statement/insert_statement.h"
#include "binder/statement/select_statement.h"
#include "binder/statement/update_statement.h"
#include "common/exception.h"
#include "common/util/string_util.h"
#include "fmt/format.h"
#include "nodes/parsenodes.hpp"
#include "type/value.h"

namespace bumblebee {

auto Binder::BindInsert(duckdb_libpgquery::PGInsertStmt *pg_stmt) -> std::unique_ptr<InsertStatement> {
  if (pg_stmt->cols != nullptr) {
    throw NotImplementedException("insert only supports all columns, don't specify columns");
  }

  auto table = BindBaseTableRef(pg_stmt->relation->relname, std::nullopt);

  if (StringUtil::StartsWith(table->table_, "__")) {
    throw BinderException(fmt::format("invalid table for insert: {}", table->table_));
  }

  auto select_statement = BindSelect(reinterpret_cast<duckdb_libpgquery::PGSelectStmt *>(pg_stmt->selectStmt));

  return std::make_unique<InsertStatement>(std::move(table), std::move(select_statement));
}

auto Binder::BindDelete(duckdb_libpgquery::PGDeleteStmt *stmt) -> std::unique_ptr<DeleteStatement> {
  auto table = BindBaseTableRef(stmt->relation->relname, std::nullopt);
  auto ctx_guard = NewContext();
  scope_ = table.get();

  std::unique_ptr<BoundExpression> expr;
  if (stmt->whereClause != nullptr) {
    expr = BindExpression(stmt->whereClause);
  } else {
    expr = std::make_unique<BoundConstant>(Value{true});
  }

  return std::make_unique<DeleteStatement>(std::move(table), std::move(expr));
}

auto Binder::BindUpdate(duckdb_libpgquery::PGUpdateStmt *stmt) -> std::unique_ptr<UpdateStatement> {
  if (stmt->withClause != nullptr) {
    throw NotImplementedException("update with clause not supported yet");
  }

  if (stmt->fromClause != nullptr) {
    throw NotImplementedException("update from clause not supported yet");
  }

  auto table = BindBaseTableRef(stmt->relation->relname, std::nullopt);
  auto ctx_guard = NewContext();
  scope_ = table.get();

  std::unique_ptr<BoundExpression> filter_expr;
  if (stmt->whereClause != nullptr) {
    filter_expr = BindExpression(stmt->whereClause);
  } else {
    filter_expr = std::make_unique<BoundConstant>(Value{true});
  }

  auto *root = stmt->targetList;
  std::vector<std::pair<std::unique_ptr<BoundColumnRef>, std::unique_ptr<BoundExpression>>> target_expr;

  for (auto cell = root->head; cell != nullptr; cell = cell->next) {
    auto *target = reinterpret_cast<duckdb_libpgquery::PGResTarget *>(cell->data.ptr_value);
    auto column = ResolveColumnRefFromBaseTableRef(*table, std::vector{std::string{target->name}});
    if (column == nullptr) {
      throw BinderException(fmt::format("column {} not found", target->name));
    }
    target_expr.emplace_back(std::move(column), BindExpression(target->val));
  }

  return std::make_unique<UpdateStatement>(std::move(table), std::move(filter_expr), std::move(target_expr));
}

}  // namespace bumblebee
