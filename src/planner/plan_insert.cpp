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
#include "execution/expressions/cast_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/delete_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/insert_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/update_plan.h"
#include "planner/planner.h"

namespace bumblebee {

namespace {

/** @brief The schema of a DML statement's result: how many rows it touched. */
auto MakeDmlResultSchema(const char *column_name) -> SchemaRef {
  return std::make_shared<Schema>(
      std::vector{Column{column_name, LogicalType(LogicalTypeId::INTEGER)}});
}

/** @return Whether `t` is a number BumbleBee will implicitly widen between (integers, float, decimal). */
auto IsNumericType(const LogicalType &t) -> bool {
  switch (t.GetTypeId()) {
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::DECIMAL:
      return true;
    default:
      return false;
  }
}

/**
 * @brief Whether a value of type `from` may be stored into a column of type `to`.
 *
 * Allowed: the same type, an untyped NULL (UNKNOWN — the cast is a NULL broadcast of the target, so
 * it fits any column), or a lossless numeric widening (INT -> BIGINT, INT -> DOUBLE, ...). Narrowing
 * and cross-family assignments need an explicit CAST and are rejected. `CommonType(from, to) == to`
 * means `to` is the wider supertype.
 */
auto CanAssign(const LogicalType &from, const LogicalType &to) -> bool {
  // String literals coerce into calendar columns by parsing ("2024-01-01" -> DATE); the cast is
  // strict, so a malformed literal errors instead of silently landing NULL.
  const bool string_to_calendar =
      from.GetTypeId() == LogicalTypeId::STRING &&
      (to.GetTypeId() == LogicalTypeId::DATE || to.GetTypeId() == LogicalTypeId::TIMESTAMP);
  return from == to || from.GetTypeId() == LogicalTypeId::UNKNOWN || string_to_calendar ||
         (IsNumericType(from) && IsNumericType(to) && LogicalType::CommonType(from, to) == to);
}

}  // namespace

auto Planner::PlanInsert(const InsertStatement &statement) -> AbstractPlanNodeRef {
  auto select = PlanSelect(*statement.select_);

  // The rows being inserted must line up with the table, column for column — except the auto-generated
  // `_id` primary key (column 0), which the INSERT executor fills, so the VALUES supply the rest.
  const auto &table_columns = statement.table_->schema_.GetColumns();
  const auto &child_columns = select->OutputSchema().GetColumns();
  const bool auto_id = !table_columns.empty() && table_columns.front().GetName() == AUTO_ID_COLUMN;
  const size_t offset = auto_id ? 1 : 0;

  if (child_columns.size() != table_columns.size() - offset) {
    throw PlannerException(
        fmt::format("the values do not match the schema of table {}", statement.table_->table_));
  }

  // Each supplied column must match its target column's type or widen losslessly to it. Where a widening
  // is needed (e.g. an INT literal into a BIGINT column), project the value through a CastExpression so
  // the row reaches the heap at the column's physical width; matching columns pass through untouched.
  std::vector<AbstractExpressionRef> proj_exprs;
  proj_exprs.reserve(child_columns.size());
  bool needs_cast = false;
  for (size_t c = 0; c < child_columns.size(); c++) {
    const auto &from = child_columns[c].GetType();
    const auto &to = table_columns[c + offset].GetType();
    if (!CanAssign(from, to)) {
      throw PlannerException(
          fmt::format("the values do not match the schema of table {}", statement.table_->table_));
    }
    AbstractExpressionRef col =
        std::make_shared<ColumnValueExpression>(0, static_cast<uint32_t>(c), child_columns[c]);
    if (from != to) {
      // Strict: the allowed coercions either cannot fail (lossless widenings, NULL broadcast) or
      // must error loudly when they do (string -> DATE/TIMESTAMP parses).
      col = std::make_shared<CastExpression>(std::move(col), to, /*strict=*/true);
      needs_cast = true;
    }
    proj_exprs.push_back(std::move(col));
  }
  if (needs_cast) {
    auto proj_schema = std::make_shared<Schema>(ProjectionPlanNode::InferProjectionSchema(proj_exprs));
    select = std::make_shared<ProjectionPlanNode>(std::move(proj_schema), std::move(proj_exprs), std::move(select));
  }

  return std::make_shared<InsertPlanNode>(MakeDmlResultSchema("__bumblebee_internal.insert_rows"),
                                          std::move(select), statement.table_->oid_);
}

auto Planner::PlanDelete(const DeleteStatement &statement) -> AbstractPlanNodeRef {
  auto table = PlanTableRef(*statement.table_);
  // Same WHERE handling as SELECT, so `DELETE ... WHERE k IN (SELECT ...)` flattens to a SEMI join
  // over the scan instead of failing. A SEMI join emits each qualifying row exactly once, which is
  // what a delete needs, and its output schema is its left child's, so the RID column the physical
  // lowering appends to the scan rides through untouched.
  auto rows = PlanWhere(*statement.expr_, std::move(table), /*outer_statement=*/nullptr);

  return std::make_shared<DeletePlanNode>(MakeDmlResultSchema("__bumblebee_internal.delete_rows"),
                                          std::move(rows), statement.table_->oid_);
}

auto Planner::PlanUpdate(const UpdateStatement &statement) -> AbstractPlanNodeRef {
  auto table = PlanTableRef(*statement.table_);
  // See PlanDelete: an IN/EXISTS conjunct becomes a SEMI/ANTI join over the scan. The SET
  // expressions below are planned against this node's output, whose schema is the table's either
  // way (a SEMI join emits its left side only), so nothing downstream changes shape.
  AbstractPlanNodeRef filter = PlanWhere(*statement.filter_expr_, std::move(table),
                                         /*outer_statement=*/nullptr);

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

  // A SET value may be produced at a narrower type than its column (e.g. `SET big = 1`, an INT into a
  // BIGINT). Widen it through a CastExpression so the replacement row is written at the column's width;
  // the unchanged columns already reference their old value at the column type, so they never need one.
  for (size_t idx = 0; idx < target_exprs.size(); idx++) {
    // Copy into values: GetReturnType() returns a Column by value, so a reference into its .GetType()
    // would dangle.
    const LogicalType to = filter->output_schema_->GetColumn(idx).GetType();
    const LogicalType from = target_exprs[idx]->GetReturnType().GetType();
    if (from == to) {
      continue;
    }
    if (!CanAssign(from, to)) {
      throw PlannerException(fmt::format("cannot assign a {} value to column '{}' of type {}", from.ToString(),
                                         filter->output_schema_->GetColumn(idx).GetName(), to.ToString()));
    }
    target_exprs[idx] = std::make_shared<CastExpression>(std::move(target_exprs[idx]), to);
  }

  return std::make_shared<UpdatePlanNode>(MakeDmlResultSchema("__bumblebee_internal.update_rows"),
                                          std::move(filter), statement.table_->oid_, std::move(target_exprs));
}

}  // namespace bumblebee
