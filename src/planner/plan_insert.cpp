//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// plan_insert.cpp
//
// Identification: src/planner/plan_insert.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/statement/delete_statement.h"
#include "binder/statement/insert_statement.h"
#include "binder/statement/select_statement.h"
#include "binder/statement/update_statement.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/delete_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/insert_plan.h"
#include "execution/plans/update_plan.h"
#include "planner/planner.h"

namespace bumblebee {

namespace {

/** @brief The schema of a DML statement's result: how many rows it touched. */
auto MakeDmlResultSchema(const char *column_name) -> SchemaRef {
  return std::make_shared<Schema>(
      std::vector{Column{column_name, LogicalType(LogicalTypeId::INTEGER)}});
}

}  // namespace

auto Planner::PlanInsert(const InsertStatement &statement) -> AbstractPlanNodeRef {
  auto select = PlanSelect(*statement.select_);

  // The rows being inserted must line up with the table, column for column.
  const auto &table_columns = statement.table_->schema_.GetColumns();
  const auto &child_columns = select->OutputSchema().GetColumns();
  if (!std::equal(table_columns.cbegin(), table_columns.cend(), child_columns.cbegin(), child_columns.cend(),
                  [](auto &&col1, auto &&col2) { return col1.GetType() == col2.GetType(); })) {
    throw PlannerException(
        fmt::format("the values do not match the schema of table {}", statement.table_->table_));
  }

  return std::make_shared<InsertPlanNode>(MakeDmlResultSchema("__bumblebee_internal.insert_rows"),
                                          std::move(select), statement.table_->oid_);
}

auto Planner::PlanDelete(const DeleteStatement &statement) -> AbstractPlanNodeRef {
  auto table = PlanTableRef(*statement.table_);
  auto [_, condition] = PlanExpression(*statement.expr_, {table});
  auto filter =
      std::make_shared<FilterPlanNode>(table->output_schema_, std::move(condition), std::move(table));

  return std::make_shared<DeletePlanNode>(MakeDmlResultSchema("__bumblebee_internal.delete_rows"),
                                          std::move(filter), statement.table_->oid_);
}

auto Planner::PlanUpdate(const UpdateStatement &statement) -> AbstractPlanNodeRef {
  auto table = PlanTableRef(*statement.table_);
  auto [_, condition] = PlanExpression(*statement.filter_expr_, {table});
  AbstractPlanNodeRef filter =
      std::make_shared<FilterPlanNode>(table->output_schema_, std::move(condition), std::move(table));

  const auto scope = std::vector{filter};

  // The update node carries one expression per table column, not just the ones named
  // in the SET clause: the columns not being changed get a plain reference to their
  // own old value, so the node always produces a whole row.
  std::vector<AbstractExpressionRef> target_exprs;
  target_exprs.resize(filter->output_schema_->GetColumnCount());

  for (const auto &[col, target_expr] : statement.target_expr_) {
    auto [_1, target_abstract_expr] = PlanExpression(*target_expr, scope);
    auto [_2, col_abstract_expr] = PlanColumnRef(*col, scope);
    target_exprs[col_abstract_expr->GetColIdx()] = std::move(target_abstract_expr);
  }

  for (size_t idx = 0; idx < target_exprs.size(); idx++) {
    if (target_exprs[idx] == nullptr) {
      target_exprs[idx] =
          std::make_shared<ColumnValueExpression>(0, idx, filter->output_schema_->GetColumn(idx));
    }
  }

  return std::make_shared<UpdatePlanNode>(MakeDmlResultSchema("__bumblebee_internal.update_rows"),
                                          std::move(filter), statement.table_->oid_, std::move(target_exprs));
}

}  // namespace bumblebee
