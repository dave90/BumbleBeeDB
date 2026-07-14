//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// plan_node.cpp
//
// Identification: src/execution/plans/plan_node.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <string>
#include <vector>

#include "binder/table_ref/bound_base_table_ref.h"
#include "common/util/string_util.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/topn_plan.h"
#include "execution/plans/update_plan.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

namespace bumblebee {

auto AbstractPlanNode::ChildrenToString(int indent, bool with_schema) const -> std::string {
  if (children_.empty()) {
    return "";
  }
  std::vector<std::string> children_str;
  children_str.reserve(children_.size());
  const auto indent_str = StringUtil::Indent(indent);
  for (const auto &child : children_) {
    auto child_str = child->ToString(with_schema);
    children_str.push_back(StringUtil::IndentAllLines(child_str, indent, true));
  }
  return fmt::format("\n{}{}", indent_str, fmt::join(children_str, "\n" + indent_str));
}

// ---------------------------------------------------------------------------
// Schema inference
// ---------------------------------------------------------------------------

auto SeqScanPlanNode::InferScanSchema(const BoundBaseTableRef &table_ref) -> Schema {
  std::vector<Column> schema;
  for (const auto &column : table_ref.schema_.GetColumns()) {
    // Qualify each column with the table's (possibly aliased) name, so that the two
    // sides of a self-join stay distinguishable in the plan output.
    auto col_name = fmt::format("{}.{}", table_ref.GetBoundTableName(), column.GetName());
    schema.emplace_back(col_name, column);
  }
  return Schema(schema);
}

auto NestedLoopJoinPlanNode::InferJoinSchema(const AbstractPlanNode &left, const AbstractPlanNode &right)
    -> Schema {
  std::vector<Column> columns;
  columns.reserve(left.OutputSchema().GetColumnCount() + right.OutputSchema().GetColumnCount());
  for (const auto &column : left.OutputSchema().GetColumns()) {
    columns.emplace_back(column);
  }
  for (const auto &column : right.OutputSchema().GetColumns()) {
    columns.emplace_back(column);
  }
  return Schema(columns);
}

auto ProjectionPlanNode::InferProjectionSchema(const std::vector<AbstractExpressionRef> &expressions) -> Schema {
  std::vector<Column> schema;
  schema.reserve(expressions.size());
  for (const auto &expr : expressions) {
    schema.emplace_back(expr->GetReturnType());
  }
  return Schema(schema);
}

auto ProjectionPlanNode::RenameSchema(const Schema &schema, const std::vector<std::string> &col_names) -> Schema {
  BUMBLEBEE_ASSERT(schema.GetColumnCount() == col_names.size(),
                   "column count mismatch when renaming a schema");
  std::vector<Column> columns;
  columns.reserve(schema.GetColumnCount());
  for (uint32_t i = 0; i < schema.GetColumnCount(); i++) {
    columns.emplace_back(schema.GetColumn(i).WithColumnName(col_names[i]));
  }
  return Schema(columns);
}

auto AggregationPlanNode::InferAggSchema(const std::vector<AbstractExpressionRef> &group_bys,
                                         const std::vector<AbstractExpressionRef> &aggregates,
                                         const std::vector<AggregationType> &agg_types) -> Schema {
  BUMBLEBEE_ASSERT(aggregates.size() == agg_types.size(),
                   "there must be one aggregate type per aggregated expression");
  std::vector<Column> columns;
  columns.reserve(group_bys.size() + aggregates.size());

  // The group-by columns come first, then the aggregate columns.
  for (const auto &group_by : group_bys) {
    columns.emplace_back(group_by->GetReturnType());
  }
  for (size_t idx = 0; idx < aggregates.size(); idx++) {
    switch (agg_types[idx]) {
      case AggregationType::CountStarAggregate:
      case AggregationType::CountAggregate:
        // A count is an integer no matter what it counts.
        columns.emplace_back(Column::Make("<unnamed>", LogicalType(LogicalTypeId::INTEGER)));
        break;
      case AggregationType::SumAggregate:
      case AggregationType::MinAggregate:
      case AggregationType::MaxAggregate:
        // These preserve the type of what they aggregate.
        columns.emplace_back(aggregates[idx]->GetReturnType());
        break;
      default:
        UNREACHABLE("unknown aggregation type");
    }
  }
  return Schema(columns);
}

// ---------------------------------------------------------------------------
// Out-of-line PlanNodeToString
// ---------------------------------------------------------------------------

auto ProjectionPlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("Projection {{ exprs={} }}", expressions_);
}

auto AggregationPlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("Agg {{ types={}, aggregates={}, group_by={} }}", agg_types_, aggregates_, group_bys_);
}

auto NestedLoopJoinPlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("NestedLoopJoin {{ type={}, predicate={} }}", join_type_, predicate_);
}

auto HashJoinPlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("HashJoin {{ type={}, left_key={}, right_key={} }}", join_type_, left_key_expressions_,
                     right_key_expressions_);
}

auto SortPlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("Sort {{ order_bys={} }}", order_bys_);
}

auto TopNPlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("TopN {{ n={}, order_bys={} }}", n_, order_bys_);
}

auto UpdatePlanNode::PlanNodeToString() const -> std::string {
  return fmt::format("Update {{ table_oid={}, target_exprs={} }}", table_oid_, target_expressions_);
}

}  // namespace bumblebee
