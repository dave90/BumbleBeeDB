//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bind_drop.cpp
//
// Identification: src/binder/bind_drop.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/statement/drop_statement.h"
#include "common/exception.h"
#include "nodes/nodes.hpp"
#include "nodes/parsenodes.hpp"

namespace bumblebee {

auto Binder::BindDrop(duckdb_libpgquery::PGDropStmt *stmt) -> std::unique_ptr<DropStatement> {
  // BumbleBeeDB only knows about tables; DROP INDEX / VIEW / SCHEMA / ... are not supported yet.
  if (stmt->removeType != duckdb_libpgquery::PG_OBJECT_TABLE) {
    throw NotImplementedException("only DROP TABLE is supported");
  }

  std::vector<std::string> tables;
  for (auto *cell = stmt->objects->head; cell != nullptr; cell = cell->next) {
    // Each object is a list of name components (schema, table). We only support unqualified names, so
    // the last component is the table name.
    auto *name_list = reinterpret_cast<duckdb_libpgquery::PGList *>(cell->data.ptr_value);
    auto *last = reinterpret_cast<duckdb_libpgquery::PGValue *>(name_list->tail->data.ptr_value);
    tables.emplace_back(last->val.str);
  }

  return std::make_unique<DropStatement>(std::move(tables), stmt->missing_ok);
}

}  // namespace bumblebee
