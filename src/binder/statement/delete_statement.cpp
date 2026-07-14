//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// delete_statement.cpp
//
// Identification: src/binder/statement/delete_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/statement/delete_statement.h"

#include <memory>
#include <string>
#include <utility>

#include "fmt/format.h"

namespace bumblebee {

DeleteStatement::DeleteStatement(std::unique_ptr<BoundBaseTableRef> table, std::unique_ptr<BoundExpression> expr)
    : BoundStatement(StatementType::DELETE_STATEMENT), table_(std::move(table)), expr_(std::move(expr)) {}

auto DeleteStatement::ToString() const -> std::string {
  return fmt::format("BoundDelete {{ table={}, expr={} }}", *table_, *expr_);
}

}  // namespace bumblebee
