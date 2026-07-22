//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transaction_statement.cpp
//
// Identification: src/binder/statement/transaction_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/statement/transaction_statement.h"

#include <string>

#include "fmt/format.h"

namespace bumblebee {

auto TransactionTypeToString(TransactionType type) -> std::string {
  switch (type) {
    case TransactionType::BEGIN:
      return "BEGIN";
    case TransactionType::COMMIT:
      return "COMMIT";
    case TransactionType::ROLLBACK:
      return "ROLLBACK";
  }
  return "UNKNOWN";
}

TransactionStatement::TransactionStatement(TransactionType txn_type)
    : BoundStatement(StatementType::TRANSACTION_STATEMENT), txn_type_(txn_type) {}

auto TransactionStatement::ToString() const -> std::string {
  return fmt::format("BoundTransaction {{ {} }}", TransactionTypeToString(txn_type_));
}

}  // namespace bumblebee
