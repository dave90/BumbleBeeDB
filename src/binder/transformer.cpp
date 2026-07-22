//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transformer.cpp
//
// Identification: src/binder/transformer.cpp
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

#include "binder/binder.h"
#include "binder/bound_statement.h"
#include "binder/statement/create_statement.h"
#include "binder/statement/delete_statement.h"
#include "binder/statement/drop_statement.h"
#include "binder/statement/transaction_statement.h"
#include "binder/statement/explain_statement.h"
#include "binder/statement/insert_statement.h"
#include "binder/statement/select_statement.h"
#include "binder/statement/update_statement.h"
#include "common/exception.h"
#include "nodes/nodes.hpp"
#include "nodes/parsenodes.hpp"

namespace bumblebee {

void Binder::SaveParseTree(duckdb_libpgquery::PGList *tree) {
  for (auto entry = tree->head; entry != nullptr; entry = entry->next) {
    statement_nodes_.push_back(reinterpret_cast<duckdb_libpgquery::PGNode *>(entry->data.ptr_value));
  }
}

auto Binder::BindStatement(duckdb_libpgquery::PGNode *stmt) -> std::unique_ptr<BoundStatement> {
  switch (stmt->type) {
    case duckdb_libpgquery::T_PGRawStmt:
      return BindStatement(reinterpret_cast<duckdb_libpgquery::PGRawStmt *>(stmt)->stmt);
    case duckdb_libpgquery::T_PGCreateStmt:
      return BindCreate(reinterpret_cast<duckdb_libpgquery::PGCreateStmt *>(stmt));
    case duckdb_libpgquery::T_PGDropStmt:
      return BindDrop(reinterpret_cast<duckdb_libpgquery::PGDropStmt *>(stmt));
    case duckdb_libpgquery::T_PGTransactionStmt:
      return BindTransaction(reinterpret_cast<duckdb_libpgquery::PGTransactionStmt *>(stmt));
    case duckdb_libpgquery::T_PGInsertStmt:
      return BindInsert(reinterpret_cast<duckdb_libpgquery::PGInsertStmt *>(stmt));
    case duckdb_libpgquery::T_PGSelectStmt:
      return BindSelect(reinterpret_cast<duckdb_libpgquery::PGSelectStmt *>(stmt));
    case duckdb_libpgquery::T_PGExplainStmt:
      return BindExplain(reinterpret_cast<duckdb_libpgquery::PGExplainStmt *>(stmt));
    case duckdb_libpgquery::T_PGDeleteStmt:
      return BindDelete(reinterpret_cast<duckdb_libpgquery::PGDeleteStmt *>(stmt));
    case duckdb_libpgquery::T_PGUpdateStmt:
      return BindUpdate(reinterpret_cast<duckdb_libpgquery::PGUpdateStmt *>(stmt));
    default:
      throw NotImplementedException(NodeTagToString(stmt->type));
  }
}

}  // namespace bumblebee
