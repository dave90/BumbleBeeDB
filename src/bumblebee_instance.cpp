//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumblebee_instance.cpp
//
// Identification: src/bumblebee_instance.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "bumblebee_instance.h"

#include <memory>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_statement.h"
#include "binder/statement/create_statement.h"
#include "binder/statement/explain_statement.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/enums/statement_type.h"
#include "common/exception.h"
#include "common/util/string_util.h"
#include "fmt/format.h"
#include "optimizer/optimizer.h"
#include "planner/planner.h"
#include "type/logical_type.h"

namespace bumblebee {

BumbleBeeInstance::BumbleBeeInstance() : catalog_(std::make_unique<Catalog>()) {}

BumbleBeeInstance::~BumbleBeeInstance() = default;

void BumbleBeeInstance::WriteOneCell(const std::string &cell, ResultWriter &writer) { writer.OneCell(cell); }

void BumbleBeeInstance::GenerateMockTable() {
  const auto int_type = LogicalType(LogicalTypeId::INTEGER);
  catalog_->CreateTable("__mock_table_1",
                        Schema(std::vector{Column{"colA", int_type}, Column{"colB", int_type}}));
  catalog_->CreateTable("__mock_table_2",
                        Schema(std::vector{Column{"colC", int_type}, Column{"colD", int_type}}));
}

void BumbleBeeInstance::CmdDisplayTables(ResultWriter &writer) {
  auto table_names = catalog_->GetTableNames();
  std::sort(table_names.begin(), table_names.end());

  writer.BeginTable(false);
  writer.BeginHeader();
  writer.WriteHeaderCell("oid");
  writer.WriteHeaderCell("name");
  writer.WriteHeaderCell("cols");
  writer.EndHeader();
  for (const auto &name : table_names) {
    const auto table_info = catalog_->GetTable(name);
    writer.BeginRow();
    writer.WriteCell(fmt::format("{}", table_info->oid_));
    writer.WriteCell(table_info->name_);
    writer.WriteCell(table_info->schema_.ToString());
    writer.EndRow();
  }
  writer.EndTable();
}

void BumbleBeeInstance::CmdDisplayHelp(ResultWriter &writer) {
  std::string help = R"(Welcome to the BumbleBeeDB shell!

\dt: show all tables
\help: show this message again

BumbleBeeDB has no execution engine yet. CREATE TABLE registers a schema in the
catalog; every other statement prints the plan it would have run. Use EXPLAIN to
see the binder and planner output alongside the optimized plan.
)";
  WriteOneCell(help, writer);
}

void BumbleBeeInstance::HandleCreateStatement(const CreateStatement &stmt, ResultWriter &writer) {
  auto info = catalog_->CreateTable(stmt.table_, Schema(stmt.columns_));
  if (info == NULL_TABLE_INFO) {
    throw Exception(fmt::format("failed to create table {}: it already exists", stmt.table_));
  }
  WriteOneCell(fmt::format("Table created with id = {}", info->oid_), writer);
}

void BumbleBeeInstance::HandleExplainStatement(const ExplainStatement &stmt, ResultWriter &writer) {
  std::string output;

  if ((stmt.options_ & ExplainOptions::BINDER) != 0) {
    output += "=== BINDER ===\n";
    output += stmt.statement_->ToString();
    output += "\n";
  }

  Planner planner(*catalog_);
  planner.PlanQuery(*stmt.statement_);

  const bool show_schema = (stmt.options_ & ExplainOptions::SCHEMA) != 0;

  if ((stmt.options_ & ExplainOptions::PLANNER) != 0) {
    output += "=== PLANNER ===\n";
    output += planner.plan_->ToString(show_schema);
    output += "\n";
  }

  Optimizer optimizer(*catalog_);
  auto optimized_plan = optimizer.Optimize(planner.plan_);

  if ((stmt.options_ & ExplainOptions::OPTIMIZER) != 0) {
    output += "=== OPTIMIZER ===\n";
    output += optimized_plan->ToString(show_schema);
    output += "\n";
  }

  WriteOneCell(output, writer);
}

auto BumbleBeeInstance::ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool {
  if (!sql.empty() && sql[0] == '\\') {
    if (sql == "\\dt") {
      CmdDisplayTables(writer);
      return true;
    }
    if (sql == "\\help") {
      CmdDisplayHelp(writer);
      return true;
    }
    throw Exception(fmt::format("unsupported meta-command: {}", sql));
  }

  Binder binder(*catalog_);
  binder.ParseAndSave(sql);

  for (auto *stmt : binder.statement_nodes_) {
    auto statement = binder.BindStatement(stmt);

    switch (statement->type_) {
      case StatementType::CREATE_STATEMENT: {
        HandleCreateStatement(dynamic_cast<const CreateStatement &>(*statement), writer);
        continue;
      }
      case StatementType::EXPLAIN_STATEMENT: {
        HandleExplainStatement(dynamic_cast<const ExplainStatement &>(*statement), writer);
        continue;
      }
      default:
        break;
    }

    Planner planner(*catalog_);
    planner.PlanQuery(*statement);

    Optimizer optimizer(*catalog_);
    auto optimized_plan = optimizer.Optimize(planner.plan_);

    // There is no execution engine yet, so the plan *is* the result. Once the
    // engine lands this becomes an Execute() call and the rows go to the writer.
    WriteOneCell(fmt::format("=== OPTIMIZED PLAN (no execution engine yet) ===\n{}",
                             optimized_plan->ToString(true)),
                 writer);
  }

  return true;
}

}  // namespace bumblebee
