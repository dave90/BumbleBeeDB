//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bind_transaction.cpp
//
// Identification: src/binder/bind_transaction.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>

#include "binder/binder.h"
#include "binder/statement/transaction_statement.h"
#include "common/exception.h"
#include "nodes/nodes.hpp"
#include "nodes/parsenodes.hpp"

namespace bumblebee {

auto Binder::BindTransaction(duckdb_libpgquery::PGTransactionStmt *stmt) -> std::unique_ptr<TransactionStatement> {
  switch (stmt->kind) {
    case duckdb_libpgquery::PG_TRANS_STMT_BEGIN:
    case duckdb_libpgquery::PG_TRANS_STMT_START:  // START TRANSACTION is a synonym for BEGIN
      return std::make_unique<TransactionStatement>(TransactionType::BEGIN);
    case duckdb_libpgquery::PG_TRANS_STMT_COMMIT:
      return std::make_unique<TransactionStatement>(TransactionType::COMMIT);
    case duckdb_libpgquery::PG_TRANS_STMT_ROLLBACK:
      return std::make_unique<TransactionStatement>(TransactionType::ROLLBACK);
    default:
      // Savepoints, prepared / two-phase commit, etc. are not supported.
      throw NotImplementedException("unsupported transaction command");
  }
}

}  // namespace bumblebee
