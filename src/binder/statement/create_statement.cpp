//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// create_statement.cpp
//
// Identification: src/binder/statement/create_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/statement/create_statement.h"

#include <string>
#include <utility>
#include <vector>

#include "catalog/column.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

namespace bumblebee {

CreateStatement::CreateStatement(std::string table, std::vector<Column> columns, std::vector<std::string> primary_key)
    : BoundStatement(StatementType::CREATE_STATEMENT),
      table_(std::move(table)),
      columns_(std::move(columns)),
      primary_key_(std::move(primary_key)) {}

auto CreateStatement::ToString() const -> std::string {
  return fmt::format("BoundCreate {{\n  table={}\n  columns={}\n  primary_key={}\n}}", table_, columns_, primary_key_);
}

}  // namespace bumblebee
