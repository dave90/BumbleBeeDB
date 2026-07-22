//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// drop_statement.cpp
//
// Identification: src/binder/statement/drop_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/statement/drop_statement.h"

#include <string>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "fmt/ranges.h"

namespace bumblebee {

DropStatement::DropStatement(std::vector<std::string> tables, bool if_exists)
    : BoundStatement(StatementType::DROP_STATEMENT), tables_(std::move(tables)), if_exists_(if_exists) {}

auto DropStatement::ToString() const -> std::string {
  return fmt::format("BoundDrop {{\n  tables={}\n  if_exists={}\n}}", tables_, if_exists_);
}

}  // namespace bumblebee
