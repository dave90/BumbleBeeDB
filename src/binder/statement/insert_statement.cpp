//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// insert_statement.cpp
//
// Identification: src/binder/statement/insert_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/statement/insert_statement.h"

#include <memory>
#include <string>
#include <utility>

#include "binder/statement/select_statement.h"
#include "common/util/string_util.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

namespace bumblebee {

InsertStatement::InsertStatement(std::unique_ptr<BoundBaseTableRef> table, std::unique_ptr<SelectStatement> select)
    : BoundStatement(StatementType::INSERT_STATEMENT), table_(std::move(table)), select_(std::move(select)) {}

auto InsertStatement::ToString() const -> std::string {
  return fmt::format("BoundInsert {{\n  table={},\n  select={}\n}}", *table_,
                     StringUtil::IndentAllLines(select_->ToString(), 2));
}

}  // namespace bumblebee
