//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// plan_table_ref.cpp
//
// Identification: src/planner/plan_table_ref.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_table_ref.h"
#include "binder/statement/select_statement.h"
#include "binder/table_ref/bound_base_table_ref.h"
#include "binder/table_ref/bound_cross_product_ref.h"
#include "binder/table_ref/bound_cte_ref.h"
#include "binder/table_ref/bound_expression_list_ref.h"
#include "binder/table_ref/bound_join_ref.h"
#include "binder/table_ref/bound_subquery_ref.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/values_plan.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "planner/planner.h"

namespace bumblebee {

auto Planner::PlanTableRef(const BoundTableRef &table_ref) -> AbstractPlanNodeRef {
  switch (table_ref.type_) {
    case TableReferenceType::BASE_TABLE:
      return PlanBaseTableRef(dynamic_cast<const BoundBaseTableRef &>(table_ref));
    case TableReferenceType::CROSS_PRODUCT:
      return PlanCrossProductRef(dynamic_cast<const BoundCrossProductRef &>(table_ref));
    case TableReferenceType::JOIN:
      return PlanJoinRef(dynamic_cast<const BoundJoinRef &>(table_ref));
    case TableReferenceType::EXPRESSION_LIST:
      return PlanExpressionListRef(dynamic_cast<const BoundExpressionListRef &>(table_ref));
    case TableReferenceType::SUBQUERY: {
      const auto &subquery = dynamic_cast<const BoundSubqueryRef &>(table_ref);
      return PlanSubquery(subquery, subquery.alias_);
    }
    case TableReferenceType::CTE:
      return PlanCTERef(dynamic_cast<const BoundCTERef &>(table_ref));
    default:
      break;
  }
  throw PlannerException(fmt::format("the table ref type {} cannot be planned", table_ref.type_));
}

auto Planner::PlanBaseTableRef(const BoundBaseTableRef &table_ref) -> AbstractPlanNodeRef {
  // The scan always produces every column of the table; a Projection above it drops
  // the ones the query doesn't want. That keeps planning simple, and the optimizer's
  // column-pruning rule is where the unused columns get removed.
  auto table = catalog_.GetTable(table_ref.table_);
  BUMBLEBEE_ASSERT(table != NULL_TABLE_INFO, "the binder should have rejected an unknown table");

  return std::make_shared<SeqScanPlanNode>(std::make_shared<Schema>(SeqScanPlanNode::InferScanSchema(table_ref)),
                                           table->oid_, table->name_);
}

auto Planner::PlanSubquery(const BoundSubqueryRef &table_ref, const std::string &alias) -> AbstractPlanNodeRef {
  auto select_node = PlanSelect(*table_ref.subquery_);

  // A projection that only renames: it prefixes every column with the subquery's
  // alias so `sub.x` resolves. It computes nothing, so OptimizeMergeProjection
  // removes it again — it exists purely to carry the names.
  std::vector<std::string> output_column_names;
  std::vector<AbstractExpressionRef> exprs;
  size_t idx = 0;
  for (const auto &col : select_node->OutputSchema().GetColumns()) {
    exprs.push_back(std::make_shared<ColumnValueExpression>(0, idx, col));
    output_column_names.emplace_back(
        fmt::format("{}.{}", alias, fmt::join(table_ref.select_list_name_[idx], ".")));
    idx++;
  }

  return std::make_shared<ProjectionPlanNode>(
      std::make_shared<Schema>(
          ProjectionPlanNode::RenameSchema(ProjectionPlanNode::InferProjectionSchema(exprs), output_column_names)),
      std::move(exprs), std::move(select_node));
}

auto Planner::PlanCTERef(const BoundCTERef &table_ref) -> AbstractPlanNodeRef {
  for (const auto &cte : *ctx_.cte_list_) {
    if (cte->alias_ == table_ref.cte_name_) {
      // A CTE is just a subquery planned again at each reference. There is no
      // materialization and no recursion.
      return PlanSubquery(*cte, table_ref.alias_);
    }
  }
  UNREACHABLE("the binder should have rejected an unknown CTE");
}

auto Planner::PlanCrossProductRef(const BoundCrossProductRef &table_ref) -> AbstractPlanNodeRef {
  auto left = PlanTableRef(*table_ref.left_);
  auto right = PlanTableRef(*table_ref.right_);
  // A cross product is a join whose predicate is always true. The WHERE clause is
  // planned as a separate Filter above it; the optimizer's MergeFilterNLJ rule folds
  // that filter back down into this predicate.
  return std::make_shared<NestedLoopJoinPlanNode>(
      std::make_shared<Schema>(NestedLoopJoinPlanNode::InferJoinSchema(*left, *right)), std::move(left),
      std::move(right), std::make_shared<ConstantValueExpression>(Value{true}), JoinType::INNER);
}

auto Planner::PlanJoinRef(const BoundJoinRef &table_ref) -> AbstractPlanNodeRef {
  auto left = PlanTableRef(*table_ref.left_);
  auto right = PlanTableRef(*table_ref.right_);
  auto [_, join_condition] = PlanExpression(*table_ref.condition_, {left, right});
  return std::make_shared<NestedLoopJoinPlanNode>(
      std::make_shared<Schema>(NestedLoopJoinPlanNode::InferJoinSchema(*left, *right)), std::move(left),
      std::move(right), std::move(join_condition), table_ref.join_type_);
}

auto Planner::PlanExpressionListRef(const BoundExpressionListRef &table_ref) -> AbstractPlanNodeRef {
  std::vector<std::vector<AbstractExpressionRef>> all_exprs;
  all_exprs.reserve(table_ref.values_.size());
  for (const auto &row : table_ref.values_) {
    std::vector<AbstractExpressionRef> row_exprs;
    row_exprs.reserve(row.size());
    for (const auto &col : row) {
      auto [_, expr] = PlanExpression(*col, {});
      row_exprs.push_back(std::move(expr));
    }
    all_exprs.emplace_back(std::move(row_exprs));
  }

  // The rows are all the same width, so the first one gives the schema.
  const auto &first_row = all_exprs[0];
  std::vector<Column> cols;
  cols.reserve(first_row.size());
  size_t idx = 0;
  for (const auto &col : first_row) {
    cols.emplace_back(col->GetReturnType().WithColumnName(fmt::format("{}.{}", table_ref.identifier_, idx)));
    idx++;
  }

  return std::make_shared<ValuesPlanNode>(std::make_shared<Schema>(cols), std::move(all_exprs));
}

}  // namespace bumblebee
