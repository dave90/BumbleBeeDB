//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// explain_statement.cpp
//
// Identification: src/binder/statement/explain_statement.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "binder/statement/explain_statement.h"

#include <memory>
#include <string>
#include <utility>

#include "common/util/string_util.h"
#include "fmt/format.h"

namespace bumblebee {

ExplainStatement::ExplainStatement(std::unique_ptr<BoundStatement> statement, uint8_t options)
    : BoundStatement(StatementType::EXPLAIN_STATEMENT), statement_(std::move(statement)), options_(options) {}

auto ExplainStatement::ToString() const -> std::string {
  return fmt::format("BoundExplain {{\n  statement={},\n  options={},\n}}",
                     StringUtil::IndentAllLines(statement_->ToString(), 2, true), options_);
}

}  // namespace bumblebee
