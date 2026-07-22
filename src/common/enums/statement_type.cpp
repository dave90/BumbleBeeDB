//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// statement_type.cpp
//
// Identification: src/common/enums/statement_type.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/enums/statement_type.h"

namespace bumblebee {

auto StatementTypeToString(StatementType type) -> std::string {
  switch (type) {
    case StatementType::INVALID_STATEMENT:
      return "Invalid";
    case StatementType::SELECT_STATEMENT:
      return "Select";
    case StatementType::INSERT_STATEMENT:
      return "Insert";
    case StatementType::UPDATE_STATEMENT:
      return "Update";
    case StatementType::DELETE_STATEMENT:
      return "Delete";
    case StatementType::CREATE_STATEMENT:
      return "Create";
    case StatementType::DROP_STATEMENT:
      return "Drop";
    case StatementType::EXPLAIN_STATEMENT:
      return "Explain";
    case StatementType::TRANSACTION_STATEMENT:
      return "Transaction";
  }
  return "Unknown";
}

}  // namespace bumblebee
