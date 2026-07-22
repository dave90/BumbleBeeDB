//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bind_select.cpp
//
// Identification: src/binder/bind_select.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// BindSort is derived from DuckDB, which is licensed under the MIT License.
// Copyright 2018-2022 Stichting DuckDB Foundation.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_expression.h"
#include "binder/bound_order_by.h"
#include "binder/bound_statement.h"
#include "binder/bound_table_ref.h"
#include "binder/expressions/bound_agg_call.h"
#include "binder/expressions/bound_alias.h"
#include "binder/expressions/bound_binary_op.h"
#include "binder/expressions/bound_column_ref.h"
#include "binder/expressions/bound_constant.h"
#include "binder/expressions/bound_func_call.h"
#include "binder/expressions/bound_star.h"
#include "binder/expressions/bound_unary_op.h"
#include "binder/statement/explain_statement.h"
#include "binder/statement/select_statement.h"
#include "binder/table_ref/bound_base_table_ref.h"
#include "binder/table_ref/bound_cross_product_ref.h"
#include "binder/table_ref/bound_cte_ref.h"
#include "binder/table_ref/bound_expression_list_ref.h"
#include "binder/table_ref/bound_join_ref.h"
#include "binder/table_ref/bound_subquery_ref.h"
#include "catalog/catalog.h"
#include "common/exception.h"
#include "common/macros.h"
#include "common/util/string_util.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "nodes/nodes.hpp"
#include "nodes/parsenodes.hpp"
#include "nodes/pg_list.hpp"
#include "nodes/primnodes.hpp"
#include "nodes/value.hpp"
#include "pg_definitions.hpp"
#include "type/logical_type.h"
#include "type/value.h"

namespace bumblebee {

auto Binder::BindValuesList(duckdb_libpgquery::PGList *list) -> std::unique_ptr<BoundExpressionListRef> {
  std::vector<std::vector<std::unique_ptr<BoundExpression>>> all_values;

  for (auto value_list = list->head; value_list != nullptr; value_list = value_list->next) {
    auto *target = static_cast<duckdb_libpgquery::PGList *>(value_list->data.ptr_value);

    auto values = BindExpressionList(target);

    if (!all_values.empty() && all_values[0].size() != values.size()) {
      throw BinderException("values must have the same length");
    }
    all_values.push_back(std::move(values));
  }

  if (all_values.empty()) {
    throw BinderException("at least one row of values should be provided");
  }

  return std::make_unique<BoundExpressionListRef>(std::move(all_values), "<unnamed>");
}

auto Binder::BindSubquery(duckdb_libpgquery::PGSelectStmt *node, const std::string &alias)
    -> std::unique_ptr<BoundSubqueryRef> {
  std::vector<std::vector<std::string>> select_list_name;
  auto subquery = BindSelect(node);
  for (const auto &col : subquery->select_list_) {
    switch (col->type_) {
      case ExpressionType::COLUMN_REF: {
        const auto &column_ref_expr = dynamic_cast<const BoundColumnRef &>(*col);
        select_list_name.push_back(column_ref_expr.col_name_);
        continue;
      }
      case ExpressionType::ALIAS: {
        const auto &alias_expr = dynamic_cast<const BoundAlias &>(*col);
        select_list_name.push_back(std::vector{alias_expr.alias_});
        continue;
      }
      default:
        select_list_name.push_back(std::vector{fmt::format("__item#{}", universal_id_++)});
        continue;
    }
  }
  return std::make_unique<BoundSubqueryRef>(std::move(subquery), std::move(select_list_name), alias);
}

auto Binder::BindRangeSubselect(duckdb_libpgquery::PGRangeSubselect *root) -> std::unique_ptr<BoundTableRef> {
  if (root->lateral) {
    throw NotImplementedException("LATERAL in subquery is not supported");
  }

  if (root->alias != nullptr) {
    return BindSubquery(reinterpret_cast<duckdb_libpgquery::PGSelectStmt *>(root->subquery),
                        std::string(root->alias->aliasname));
  }
  return BindSubquery(reinterpret_cast<duckdb_libpgquery::PGSelectStmt *>(root->subquery),
                      fmt::format("__subquery#{}", universal_id_++));
}

auto Binder::BindCTE(duckdb_libpgquery::PGWithClause *node) -> std::vector<std::unique_ptr<BoundSubqueryRef>> {
  std::vector<std::unique_ptr<BoundSubqueryRef>> ctes;
  for (auto cte_ele = node->ctes->head; cte_ele != nullptr; cte_ele = cte_ele->next) {
    auto *cte = reinterpret_cast<duckdb_libpgquery::PGCommonTableExpr *>(cte_ele->data.ptr_value);

    if (cte->ctequery == nullptr || cte->ctequery->type != duckdb_libpgquery::T_PGSelectStmt) {
      throw BinderException("SELECT not found in CTE");
    }

    if (cte->cterecursive || node->recursive) {
      throw NotImplementedException("recursive CTE not supported");
    }

    ctes.emplace_back(BindSubquery(reinterpret_cast<duckdb_libpgquery::PGSelectStmt *>(cte->ctequery), cte->ctename));
  }

  return ctes;
}

auto Binder::BindSelect(duckdb_libpgquery::PGSelectStmt *pg_stmt) -> std::unique_ptr<SelectStatement> {
  auto ctx_guard = NewContext();

  // Bind the VALUES clause. `VALUES (1, 2)` binds to a SELECT over an expression list.
  if (pg_stmt->valuesLists != nullptr) {
    auto values_list_name = fmt::format("__values#{}", universal_id_++);
    auto value_list = BindValuesList(pg_stmt->valuesLists);
    value_list->identifier_ = values_list_name;
    std::vector<std::unique_ptr<BoundExpression>> exprs;
    size_t expr_length = value_list->values_[0].size();
    exprs.reserve(expr_length);
    for (size_t i = 0; i < expr_length; i++) {
      exprs.emplace_back(std::make_unique<BoundColumnRef>(std::vector{values_list_name, fmt::format("{}", i)}));
    }
    return std::make_unique<SelectStatement>(
        std::move(value_list), std::move(exprs), std::make_unique<BoundExpression>(),
        std::vector<std::unique_ptr<BoundExpression>>{}, std::make_unique<BoundExpression>(),
        std::make_unique<BoundExpression>(), std::make_unique<BoundExpression>(),
        std::vector<std::unique_ptr<BoundOrderBy>>{}, CTEList{}, false);
  }

  // Bind CTEs.
  auto ctes = CTEList{};
  if (pg_stmt->withClause != nullptr) {
    ctes = BindCTE(pg_stmt->withClause);
    cte_scope_ = &ctes;
  }

  // Bind the FROM clause.
  auto table = BindFrom(pg_stmt->fromClause);
  scope_ = table.get();

  // Bind DISTINCT.
  bool is_distinct = false;
  if (pg_stmt->distinctClause != nullptr) {
    auto *target = reinterpret_cast<duckdb_libpgquery::PGNode *>(pg_stmt->distinctClause->head->data.ptr_value);
    if (target != nullptr) {
      throw NotImplementedException("DISTINCT ON is not supported");
    }
    is_distinct = true;
  }

  // Bind the SELECT list.
  if (pg_stmt->targetList == nullptr) {
    throw BinderException("no select list");
  }
  auto select_list = BindSelectList(pg_stmt->targetList);

  // Bind the WHERE clause.
  auto where = std::make_unique<BoundExpression>();
  if (pg_stmt->whereClause != nullptr) {
    where = BindWhere(pg_stmt->whereClause);
  }

  // Bind the GROUP BY clause.
  auto group_by = std::vector<std::unique_ptr<BoundExpression>>{};
  if (pg_stmt->groupClause != nullptr) {
    group_by = BindGroupBy(pg_stmt->groupClause);
  }

  // Bind the HAVING clause.
  auto having = std::make_unique<BoundExpression>();
  if (pg_stmt->havingClause != nullptr) {
    having = BindHaving(pg_stmt->havingClause);
  }

  // Bind the LIMIT clause.
  auto limit_count = std::make_unique<BoundExpression>();
  if (pg_stmt->limitCount != nullptr) {
    limit_count = BindLimitCount(pg_stmt->limitCount);
  }

  // Bind the OFFSET clause.
  auto limit_offset = std::make_unique<BoundExpression>();
  if (pg_stmt->limitOffset != nullptr) {
    limit_offset = BindLimitOffset(pg_stmt->limitOffset);
  }

  // Bind the ORDER BY clause.
  auto sort = std::vector<std::unique_ptr<BoundOrderBy>>{};
  if (pg_stmt->sortClause != nullptr) {
    sort = BindSort(pg_stmt->sortClause);
  }

  return std::make_unique<SelectStatement>(std::move(table), std::move(select_list), std::move(where),
                                           std::move(group_by), std::move(having), std::move(limit_count),
                                           std::move(limit_offset), std::move(sort), std::move(ctes), is_distinct);
}

auto Binder::BindFrom(duckdb_libpgquery::PGList *list) -> std::unique_ptr<BoundTableRef> {
  auto ctx_guard = NewContext();

  if (list == nullptr) {
    return std::make_unique<BoundTableRef>(TableReferenceType::EMPTY);
  }

  if (list->length > 1) {
    // `FROM a, b, c` is a left-deep tree of cross products.
    auto c = list->head;
    auto *lnode = reinterpret_cast<duckdb_libpgquery::PGNode *>(c->data.ptr_value);
    auto ltable = BindTableRef(lnode);
    c = lnext(c);

    auto *rnode = reinterpret_cast<duckdb_libpgquery::PGNode *>(c->data.ptr_value);
    auto rtable = BindTableRef(rnode);
    c = lnext(c);

    auto result = std::make_unique<BoundCrossProductRef>(std::move(ltable), std::move(rtable));

    for (; c != nullptr; c = lnext(c)) {
      auto *node = reinterpret_cast<duckdb_libpgquery::PGNode *>(c->data.ptr_value);
      result = std::make_unique<BoundCrossProductRef>(std::move(result), BindTableRef(node));
    }

    return result;
  }

  auto *node = reinterpret_cast<duckdb_libpgquery::PGNode *>(list->head->data.ptr_value);
  return BindTableRef(node);
}

auto Binder::BindJoin(duckdb_libpgquery::PGJoinExpr *root) -> std::unique_ptr<BoundTableRef> {
  auto ctx_guard = NewContext();
  JoinType join_type;
  switch (root->jointype) {
    case duckdb_libpgquery::PG_JOIN_INNER:
      join_type = JoinType::INNER;
      break;
    case duckdb_libpgquery::PG_JOIN_LEFT:
      join_type = JoinType::LEFT;
      break;
    case duckdb_libpgquery::PG_JOIN_FULL:
      join_type = JoinType::OUTER;
      break;
    case duckdb_libpgquery::PG_JOIN_RIGHT:
      join_type = JoinType::RIGHT;
      break;
    default:
      throw NotImplementedException(fmt::format("Join type {} not supported", static_cast<int>(root->jointype)));
  }

  auto left_table = BindTableRef(root->larg);
  auto right_table = BindTableRef(root->rarg);
  auto join_ref = std::make_unique<BoundJoinRef>(join_type, std::move(left_table), std::move(right_table), nullptr);

  // The join condition is resolved against both sides of the join, so the join itself is the scope.
  scope_ = join_ref.get();
  if (root->quals == nullptr) {
    throw BinderException("join without an ON condition is not supported; use a cross product instead");
  }
  join_ref->condition_ = BindExpression(root->quals);
  return join_ref;
}

auto Binder::BindBaseTableRef(std::string table_name, std::optional<std::string> alias)
    -> std::unique_ptr<BoundBaseTableRef> {
  auto table_info = catalog_.GetTable(table_name);
  if (table_info == nullptr) {
    throw BinderException(fmt::format("invalid table {}", table_name));
  }
  return std::make_unique<BoundBaseTableRef>(std::move(table_name), table_info->oid_, std::move(alias),
                                             table_info->schema_);
}

auto Binder::BindRangeVar(duckdb_libpgquery::PGRangeVar *table_ref) -> std::unique_ptr<BoundTableRef> {
  if (cte_scope_ != nullptr) {
    // A CTE shadows a catalog table of the same name.
    for (const auto &cte : *cte_scope_) {
      if (cte->alias_ == table_ref->relname) {
        std::string bound_name =
            table_ref->alias != nullptr ? table_ref->alias->aliasname : std::string(table_ref->relname);
        return std::make_unique<BoundCTERef>(cte->alias_, std::move(bound_name));
      }
    }
  }
  if (table_ref->alias != nullptr) {
    return BindBaseTableRef(table_ref->relname, std::make_optional(table_ref->alias->aliasname));
  }
  return BindBaseTableRef(table_ref->relname, std::nullopt);
}

auto Binder::BindTableRef(duckdb_libpgquery::PGNode *node) -> std::unique_ptr<BoundTableRef> {
  switch (node->type) {
    case duckdb_libpgquery::T_PGRangeVar:
      return BindRangeVar(reinterpret_cast<duckdb_libpgquery::PGRangeVar *>(node));
    case duckdb_libpgquery::T_PGJoinExpr:
      return BindJoin(reinterpret_cast<duckdb_libpgquery::PGJoinExpr *>(node));
    case duckdb_libpgquery::T_PGRangeSubselect:
      return BindRangeSubselect(reinterpret_cast<duckdb_libpgquery::PGRangeSubselect *>(node));
    default:
      throw NotImplementedException(fmt::format("unsupported node type: {}", Binder::NodeTagToString(node->type)));
  }
}

auto Binder::GetAllColumns(const BoundTableRef &scope) -> std::vector<std::unique_ptr<BoundExpression>> {
  switch (scope.type_) {
    case TableReferenceType::BASE_TABLE: {
      const auto &base_table_ref = dynamic_cast<const BoundBaseTableRef &>(scope);
      auto bound_table_name = base_table_ref.GetBoundTableName();
      const auto &schema = base_table_ref.schema_;
      auto columns = std::vector<std::unique_ptr<BoundExpression>>{};
      columns.reserve(schema.GetColumnCount());
      for (const auto &column : schema.GetColumns()) {
        columns.push_back(std::make_unique<BoundColumnRef>(std::vector{bound_table_name, column.GetName()}));
      }
      return columns;
    }
    case TableReferenceType::CROSS_PRODUCT: {
      const auto &cross_product_ref = dynamic_cast<const BoundCrossProductRef &>(scope);
      auto columns = GetAllColumns(*cross_product_ref.left_);
      auto append_columns = GetAllColumns(*cross_product_ref.right_);
      std::copy(std::make_move_iterator(append_columns.begin()), std::make_move_iterator(append_columns.end()),
                std::back_inserter(columns));
      return columns;
    }
    case TableReferenceType::JOIN: {
      const auto &join_ref = dynamic_cast<const BoundJoinRef &>(scope);
      auto columns = GetAllColumns(*join_ref.left_);
      auto append_columns = GetAllColumns(*join_ref.right_);
      std::copy(std::make_move_iterator(append_columns.begin()), std::make_move_iterator(append_columns.end()),
                std::back_inserter(columns));
      return columns;
    }
    case TableReferenceType::SUBQUERY: {
      const auto &subquery_ref = dynamic_cast<const BoundSubqueryRef &>(scope);
      auto columns = std::vector<std::unique_ptr<BoundExpression>>{};
      columns.reserve(subquery_ref.select_list_name_.size());
      for (const auto &col_name : subquery_ref.select_list_name_) {
        columns.emplace_back(BoundColumnRef::Prepend(std::make_unique<BoundColumnRef>(col_name), subquery_ref.alias_));
      }
      return columns;
    }
    case TableReferenceType::CTE: {
      const auto &cte_ref = dynamic_cast<const BoundCTERef &>(scope);
      for (const auto &cte : *cte_scope_) {
        if (cte_ref.cte_name_ == cte->alias_) {
          auto columns = GetAllColumns(*cte);
          for (auto &column : columns) {
            auto &column_ref = dynamic_cast<BoundColumnRef &>(*column);
            column_ref.col_name_[0] = cte_ref.alias_;
          }
          return columns;
        }
      }
      UNREACHABLE("CTE not found");
    }
    default:
      throw BinderException("select * cannot be used with this table reference type");
  }
}

auto Binder::BindSelectList(duckdb_libpgquery::PGList *list) -> std::vector<std::unique_ptr<BoundExpression>> {
  auto select_list = std::vector<std::unique_ptr<BoundExpression>>{};
  bool is_select_star = false;

  for (auto node = list->head; node != nullptr; node = lnext(node)) {
    auto *target = reinterpret_cast<duckdb_libpgquery::PGNode *>(node->data.ptr_value);

    auto expr = BindExpression(target);

    if (expr->type_ == ExpressionType::STAR) {
      // `SELECT *` expands to every column of the scope.
      if (!select_list.empty()) {
        throw BinderException("select * cannot have other expressions in list");
      }
      select_list = GetAllColumns(*scope_);
      is_select_star = true;
    } else {
      if (is_select_star) {
        throw BinderException("select * cannot have other expressions in list");
      }
      select_list.push_back(std::move(expr));
    }
  }

  return select_list;
}

auto Binder::BindExpressionList(duckdb_libpgquery::PGList *list) -> std::vector<std::unique_ptr<BoundExpression>> {
  auto exprs = std::vector<std::unique_ptr<BoundExpression>>{};

  for (auto node = list->head; node != nullptr; node = lnext(node)) {
    auto *target = reinterpret_cast<duckdb_libpgquery::PGNode *>(node->data.ptr_value);

    auto expr = BindExpression(target);

    if (expr->type_ == ExpressionType::STAR) {
      throw BinderException("unsupported * in expression list");
    }

    exprs.push_back(std::move(expr));
  }

  return exprs;
}

auto Binder::BindConstant(duckdb_libpgquery::PGAConst *node) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(node != nullptr, "nullptr");
  const auto &val = node->val;
  switch (val.type) {
    case duckdb_libpgquery::T_PGInteger: {
      BUMBLEBEE_ENSURE(val.val.ival <= INT32_MAX, "value out of range");
      return std::make_unique<BoundConstant>(Value{static_cast<int32_t>(val.val.ival)});
    }
    case duckdb_libpgquery::T_PGFloat: {
      return std::make_unique<BoundConstant>(Value{std::stod(std::string(val.val.str))});
    }
    case duckdb_libpgquery::T_PGString: {
      return std::make_unique<BoundConstant>(Value{std::string(val.val.str)});
    }
    case duckdb_libpgquery::T_PGNull: {
      // An untyped NULL. The planner narrows it once it knows what it is compared against.
      return std::make_unique<BoundConstant>(Value::Null(LogicalTypeId::INTEGER));
    }
    default:
      break;
  }
  throw NotImplementedException(fmt::format("unsupported pg value: {}", Binder::NodeTagToString(val.type)));
}

auto Binder::BindColumnRef(duckdb_libpgquery::PGColumnRef *node) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(node != nullptr, "nullptr");
  auto *fields = node->fields;
  auto *head_node = static_cast<duckdb_libpgquery::PGNode *>(fields->head->data.ptr_value);
  switch (head_node->type) {
    case duckdb_libpgquery::T_PGString: {
      if (fields->length < 1) {
        throw BinderException("unexpected field length");
      }
      std::vector<std::string> column_names;
      for (auto field = fields->head; field != nullptr; field = field->next) {
        column_names.emplace_back(reinterpret_cast<duckdb_libpgquery::PGValue *>(field->data.ptr_value)->val.str);
      }
      if (scope_ == nullptr) {
        throw BinderException(fmt::format("column {} not found: no table in scope", fmt::join(column_names, ".")));
      }
      return ResolveColumn(*scope_, column_names);
    }
    case duckdb_libpgquery::T_PGAStar:
      return BindStar(reinterpret_cast<duckdb_libpgquery::PGAStar *>(head_node));
    default:
      throw NotImplementedException(
          fmt::format("ColumnRef type {} not implemented", Binder::NodeTagToString(head_node->type)));
  }
}

auto Binder::BindResTarget(duckdb_libpgquery::PGResTarget *root) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(root != nullptr, "nullptr");
  auto expr = BindExpression(root->val);
  if (expr == nullptr) {
    return nullptr;
  }
  if (root->name != nullptr) {
    return std::make_unique<BoundAlias>(root->name, std::move(expr));
  }
  return expr;
}

auto Binder::BindStar(duckdb_libpgquery::PGAStar *node) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(node != nullptr, "nullptr");
  return std::make_unique<BoundStar>();
}

auto Binder::BindFuncCall(duckdb_libpgquery::PGFuncCall *root) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(root != nullptr, "nullptr");
  auto *name = root->funcname;
  auto function_name =
      StringUtil::Lower(reinterpret_cast<duckdb_libpgquery::PGValue *>(name->head->data.ptr_value)->val.str);

  std::vector<std::unique_ptr<BoundExpression>> children;
  if (root->args != nullptr) {
    for (auto node = root->args->head; node != nullptr; node = node->next) {
      children.push_back(BindExpression(static_cast<duckdb_libpgquery::PGNode *>(node->data.ptr_value)));
    }
  }

  if (function_name == "min" || function_name == "max" || function_name == "first" || function_name == "last" ||
      function_name == "sum" || function_name == "count") {
    // `count(*)` has no arguments, and counts rows rather than values.
    if (function_name == "count" && children.empty()) {
      function_name = "count_star";
    }

    if (root->over != nullptr) {
      throw NotImplementedException("window functions are not supported");
    }

    return std::make_unique<BoundAggCall>(function_name, root->agg_distinct, std::move(children));
  }

  return std::make_unique<BoundFuncCall>(function_name, std::move(children));
}

namespace {

/**
 * @brief Resolve an unqualified column name against a schema.
 *
 * @param schema The schema to look the name up in.
 * @param col_name The dotted parts of the column name.
 * @return std::unique_ptr<BoundColumnRef> The resolved column, or null if the schema has no such column.
 */
auto ResolveColumnRefFromSchema(const Schema &schema, const std::vector<std::string> &col_name)
    -> std::unique_ptr<BoundColumnRef> {
  if (col_name.size() != 1) {
    return nullptr;
  }
  std::unique_ptr<BoundColumnRef> column_ref = nullptr;
  for (const auto &column : schema.GetColumns()) {
    if (StringUtil::Lower(column.GetName()) == col_name[0]) {
      if (column_ref != nullptr) {
        throw BinderException(fmt::format("{} is ambiguous in schema", fmt::join(col_name, ".")));
      }
      column_ref = std::make_unique<BoundColumnRef>(std::vector{column.GetName()});
    }
  }
  return column_ref;
}

/** @brief Is `suffix` the tail of `full_name`, compared case-insensitively? */
auto MatchSuffix(const std::vector<std::string> &suffix, const std::vector<std::string> &full_name) -> bool {
  std::vector<std::string> lowercase_full_name;
  lowercase_full_name.reserve(full_name.size());
  for (const auto &col : full_name) {
    lowercase_full_name.push_back(StringUtil::Lower(col));
  }
  if (suffix.size() > lowercase_full_name.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), lowercase_full_name.rbegin());
}

}  // namespace

auto Binder::ResolveColumnRefFromBaseTableRef(const BoundBaseTableRef &table_ref,
                                              const std::vector<std::string> &col_name)
    -> std::unique_ptr<BoundColumnRef> {
  auto bound_table_name = table_ref.GetBoundTableName();

  // `x` in `SELECT x FROM y` resolves directly against the schema.
  std::unique_ptr<BoundColumnRef> direct_resolved_expr =
      BoundColumnRef::Prepend(ResolveColumnRefFromSchema(table_ref.schema_, col_name), bound_table_name);

  std::unique_ptr<BoundColumnRef> strip_resolved_expr = nullptr;

  // `y.x` in `SELECT y.x FROM y` resolves once the table prefix is stripped.
  if (col_name.size() > 1 && col_name[0] == bound_table_name) {
    auto strip_column_name = col_name;
    strip_column_name.erase(strip_column_name.begin());
    strip_resolved_expr =
        BoundColumnRef::Prepend(ResolveColumnRefFromSchema(table_ref.schema_, strip_column_name), bound_table_name);
  }

  if (strip_resolved_expr != nullptr && direct_resolved_expr != nullptr) {
    throw BinderException(fmt::format("{} is ambiguous in table {}", fmt::join(col_name, "."), table_ref.table_));
  }
  if (strip_resolved_expr != nullptr) {
    return strip_resolved_expr;
  }
  return direct_resolved_expr;
}

auto Binder::ResolveColumnRefFromSelectList(const std::vector<std::vector<std::string>> &subquery_select_list,
                                            const std::vector<std::string> &col_name)
    -> std::unique_ptr<BoundColumnRef> {
  std::unique_ptr<BoundColumnRef> column_ref = nullptr;
  for (const auto &column_full_name : subquery_select_list) {
    if (MatchSuffix(col_name, column_full_name)) {
      if (column_ref != nullptr) {
        throw BinderException(fmt::format("{} is ambiguous in subquery select list", fmt::join(col_name, ".")));
      }
      column_ref = std::make_unique<BoundColumnRef>(column_full_name);
    }
  }
  return column_ref;
}

auto Binder::ResolveColumnRefFromSubqueryRef(const BoundSubqueryRef &subquery_ref, const std::string &alias,
                                             const std::vector<std::string> &col_name)
    -> std::unique_ptr<BoundColumnRef> {
  std::unique_ptr<BoundColumnRef> direct_resolved_expr = BoundColumnRef::Prepend(
      ResolveColumnRefFromSelectList(subquery_ref.select_list_name_, col_name), subquery_ref.alias_);

  std::unique_ptr<BoundColumnRef> strip_resolved_expr = nullptr;

  if (col_name.size() > 1 && col_name[0] == alias) {
    auto strip_column_name = col_name;
    strip_column_name.erase(strip_column_name.begin());
    strip_resolved_expr = BoundColumnRef::Prepend(
        ResolveColumnRefFromSelectList(subquery_ref.select_list_name_, strip_column_name), subquery_ref.alias_);
  }

  if (strip_resolved_expr != nullptr && direct_resolved_expr != nullptr) {
    throw BinderException(fmt::format("{} is ambiguous in subquery {}", fmt::join(col_name, "."), subquery_ref.alias_));
  }
  if (strip_resolved_expr != nullptr) {
    return strip_resolved_expr;
  }
  return direct_resolved_expr;
}

auto Binder::ResolveColumnInternal(const BoundTableRef &table_ref, const std::vector<std::string> &col_name)
    -> std::unique_ptr<BoundExpression> {
  switch (table_ref.type_) {
    case TableReferenceType::BASE_TABLE: {
      const auto &base_table_ref = dynamic_cast<const BoundBaseTableRef &>(table_ref);
      return ResolveColumnRefFromBaseTableRef(base_table_ref, col_name);
    }
    case TableReferenceType::CROSS_PRODUCT: {
      const auto &cross_product_ref = dynamic_cast<const BoundCrossProductRef &>(table_ref);
      auto left_column = ResolveColumnInternal(*cross_product_ref.left_, col_name);
      auto right_column = ResolveColumnInternal(*cross_product_ref.right_, col_name);
      if (left_column != nullptr && right_column != nullptr) {
        throw BinderException(fmt::format("{} is ambiguous", fmt::join(col_name, ".")));
      }
      if (left_column != nullptr) {
        return left_column;
      }
      return right_column;
    }
    case TableReferenceType::JOIN: {
      const auto &join_ref = dynamic_cast<const BoundJoinRef &>(table_ref);
      auto left_column = ResolveColumnInternal(*join_ref.left_, col_name);
      auto right_column = ResolveColumnInternal(*join_ref.right_, col_name);
      if (left_column != nullptr && right_column != nullptr) {
        throw BinderException(fmt::format("{} is ambiguous", fmt::join(col_name, ".")));
      }
      if (left_column != nullptr) {
        return left_column;
      }
      return right_column;
    }
    case TableReferenceType::SUBQUERY: {
      const auto &subquery_ref = dynamic_cast<const BoundSubqueryRef &>(table_ref);
      return ResolveColumnRefFromSubqueryRef(subquery_ref, subquery_ref.alias_, col_name);
    }
    case TableReferenceType::CTE: {
      const auto &cte_ref = dynamic_cast<const BoundCTERef &>(table_ref);
      for (const auto &cte : *cte_scope_) {
        if (cte_ref.cte_name_ == cte->alias_) {
          return ResolveColumnRefFromSubqueryRef(*cte, cte_ref.alias_, col_name);
        }
      }
      UNREACHABLE("CTE not found");
    }
    default:
      throw BinderException("unsupported table reference type");
  }
}

auto Binder::ResolveColumn(const BoundTableRef &scope, const std::vector<std::string> &col_name)
    -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(!scope.IsInvalid(), "invalid scope");
  auto expr = ResolveColumnInternal(scope, col_name);
  if (expr == nullptr) {
    throw BinderException(fmt::format("column {} not found", fmt::join(col_name, ".")));
  }
  return expr;
}

auto Binder::BindWhere(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression> {
  return BindExpression(root);
}

auto Binder::BindGroupBy(duckdb_libpgquery::PGList *list) -> std::vector<std::unique_ptr<BoundExpression>> {
  return BindExpressionList(list);
}

auto Binder::BindHaving(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression> {
  return BindExpression(root);
}

auto Binder::BindAExpr(duckdb_libpgquery::PGAExpr *root) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(root != nullptr, "nullptr");
  auto name = std::string(reinterpret_cast<duckdb_libpgquery::PGValue *>(root->name->head->data.ptr_value)->val.str);

  if (root->kind != duckdb_libpgquery::PG_AEXPR_OP) {
    throw NotImplementedException("unsupported op in AExpr");
  }

  std::unique_ptr<BoundExpression> left_expr = nullptr;
  std::unique_ptr<BoundExpression> right_expr = nullptr;

  if (root->lexpr != nullptr) {
    left_expr = BindExpression(root->lexpr);
  }
  if (root->rexpr != nullptr) {
    right_expr = BindExpression(root->rexpr);
  }

  if (left_expr != nullptr && right_expr != nullptr) {
    return std::make_unique<BoundBinaryOp>(name, std::move(left_expr), std::move(right_expr));
  }
  if (left_expr == nullptr && right_expr != nullptr) {
    return std::make_unique<BoundUnaryOp>(name, std::move(right_expr));
  }
  throw BinderException("unsupported AExpr: left == null while right != null");
}

auto Binder::BindBoolExpr(duckdb_libpgquery::PGBoolExpr *root) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(root != nullptr, "nullptr");
  switch (root->boolop) {
    case duckdb_libpgquery::PG_AND_EXPR:
    case duckdb_libpgquery::PG_OR_EXPR: {
      const std::string op_name = root->boolop == duckdb_libpgquery::PG_AND_EXPR ? "and" : "or";

      auto exprs = BindExpressionList(root->args);
      if (exprs.size() <= 1) {
        throw BinderException(fmt::format("{} should have at least 2 args", StringUtil::Upper(op_name)));
      }
      auto expr = std::make_unique<BoundBinaryOp>(op_name, std::move(exprs[0]), std::move(exprs[1]));
      for (size_t i = 2; i < exprs.size(); i++) {
        expr = std::make_unique<BoundBinaryOp>(op_name, std::move(expr), std::move(exprs[i]));
      }
      return expr;
    }
    case duckdb_libpgquery::PG_NOT_EXPR: {
      auto exprs = BindExpressionList(root->args);
      if (exprs.size() != 1) {
        throw BinderException("NOT should have 1 arg");
      }
      return std::make_unique<BoundUnaryOp>("not", std::move(exprs[0]));
    }
  }
  UNREACHABLE("We should have handled all cases!");
}

auto Binder::BindExpression(duckdb_libpgquery::PGNode *node) -> std::unique_ptr<BoundExpression> {
  BUMBLEBEE_ASSERT(node != nullptr, "nullptr");
  switch (node->type) {
    case duckdb_libpgquery::T_PGColumnRef:
      return BindColumnRef(reinterpret_cast<duckdb_libpgquery::PGColumnRef *>(node));
    case duckdb_libpgquery::T_PGAConst:
      return BindConstant(reinterpret_cast<duckdb_libpgquery::PGAConst *>(node));
    case duckdb_libpgquery::T_PGResTarget:
      return BindResTarget(reinterpret_cast<duckdb_libpgquery::PGResTarget *>(node));
    case duckdb_libpgquery::T_PGAStar:
      return BindStar(reinterpret_cast<duckdb_libpgquery::PGAStar *>(node));
    case duckdb_libpgquery::T_PGFuncCall:
      return BindFuncCall(reinterpret_cast<duckdb_libpgquery::PGFuncCall *>(node));
    case duckdb_libpgquery::T_PGAExpr:
      return BindAExpr(reinterpret_cast<duckdb_libpgquery::PGAExpr *>(node));
    case duckdb_libpgquery::T_PGBoolExpr:
      return BindBoolExpr(reinterpret_cast<duckdb_libpgquery::PGBoolExpr *>(node));
    default:
      break;
  }
  throw NotImplementedException(fmt::format("expr of type {} not implemented", Binder::NodeTagToString(node->type)));
}

auto Binder::BindLimitCount(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression> {
  return BindExpression(root);
}

auto Binder::BindLimitOffset(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression> {
  return BindExpression(root);
}

auto Binder::BindExplain(duckdb_libpgquery::PGExplainStmt *stmt) -> std::unique_ptr<ExplainStatement> {
  uint8_t explain_options =
      ExplainOptions::PLANNER | ExplainOptions::OPTIMIZER | ExplainOptions::BINDER | ExplainOptions::SCHEMA;
  if (stmt->options != nullptr) {
    explain_options = ExplainOptions::INVALID;
    for (auto node = stmt->options->head; node != nullptr; node = node->next) {
      auto *temp = reinterpret_cast<duckdb_libpgquery::PGDefElem *>(node->data.ptr_value);
      if (strcmp(temp->defname, "planner") == 0 || strcmp(temp->defname, "p") == 0) {
        explain_options |= ExplainOptions::PLANNER;
      }
      if (strcmp(temp->defname, "binder") == 0 || strcmp(temp->defname, "b") == 0) {
        explain_options |= ExplainOptions::BINDER;
      }
      if (strcmp(temp->defname, "optimizer") == 0 || strcmp(temp->defname, "o") == 0) {
        explain_options |= ExplainOptions::OPTIMIZER;
      }
      if (strcmp(temp->defname, "schema") == 0 || strcmp(temp->defname, "s") == 0) {
        explain_options |= ExplainOptions::SCHEMA;
      }
      if (strcmp(temp->defname, "physical") == 0 || strcmp(temp->defname, "x") == 0) {
        explain_options |= ExplainOptions::PHYSICAL;
      }
      if (strcmp(temp->defname, "pipelines") == 0 || strcmp(temp->defname, "l") == 0) {
        explain_options |= ExplainOptions::PIPELINES;
      }
      if (strcmp(temp->defname, "analyze") == 0 || strcmp(temp->defname, "a") == 0) {
        explain_options |= ExplainOptions::ANALYZE;
      }
    }
  }
  return std::make_unique<ExplainStatement>(BindStatement(stmt->query), explain_options);
}

//===----------------------------------------------------------------------===//
// Derived from DuckDB, which is licensed under the MIT License.
// Copyright 2018-2022 Stichting DuckDB Foundation.
//===----------------------------------------------------------------------===//

auto Binder::BindSort(duckdb_libpgquery::PGList *list) -> std::vector<std::unique_ptr<BoundOrderBy>> {
  auto order_by = std::vector<std::unique_ptr<BoundOrderBy>>{};

  for (auto node = list->head; node != nullptr; node = node->next) {
    auto *temp = reinterpret_cast<duckdb_libpgquery::PGNode *>(node->data.ptr_value);
    if (temp->type != duckdb_libpgquery::T_PGSortBy) {
      throw NotImplementedException("unsupported order by node");
    }

    auto *sort = reinterpret_cast<duckdb_libpgquery::PGSortBy *>(temp);

    OrderByType type;
    if (sort->sortby_dir == duckdb_libpgquery::PG_SORTBY_DEFAULT) {
      type = OrderByType::DEFAULT;
    } else if (sort->sortby_dir == duckdb_libpgquery::PG_SORTBY_ASC) {
      type = OrderByType::ASC;
    } else if (sort->sortby_dir == duckdb_libpgquery::PG_SORTBY_DESC) {
      type = OrderByType::DESC;
    } else {
      throw NotImplementedException("unimplemented order by type");
    }

    OrderByNullType null_order;
    if (sort->sortby_nulls == duckdb_libpgquery::PG_SORTBY_NULLS_DEFAULT) {
      null_order = OrderByNullType::DEFAULT;
    } else if (sort->sortby_nulls == duckdb_libpgquery::PG_SORTBY_NULLS_FIRST) {
      null_order = OrderByNullType::NULLS_FIRST;
    } else if (sort->sortby_nulls == duckdb_libpgquery::PG_SORTBY_NULLS_LAST) {
      null_order = OrderByNullType::NULLS_LAST;
    } else {
      throw NotImplementedException("unimplemented nulls order type");
    }

    order_by.emplace_back(std::make_unique<BoundOrderBy>(type, null_order, BindExpression(sort->node)));
  }
  return order_by;
}

//===----------------------------------------------------------------------===//
// End derived from DuckDB.
//===----------------------------------------------------------------------===//

}  // namespace bumblebee
