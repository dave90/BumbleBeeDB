//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// aggregation_plan.h
//
// Identification: src/include/execution/plans/aggregation_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"
#include "fmt/format.h"

namespace bumblebee {

/** The supported aggregate functions. */
enum class AggregationType { CountStarAggregate, CountAggregate, SumAggregate, MinAggregate, MaxAggregate };

}  // namespace bumblebee

template <>
struct fmt::formatter<bumblebee::AggregationType> : fmt::formatter<fmt::string_view> {
  template <typename FormatContext>
  auto format(bumblebee::AggregationType c, FormatContext &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::AggregationType::CountStarAggregate:
        name = "count_star";
        break;
      case bumblebee::AggregationType::CountAggregate:
        name = "count";
        break;
      case bumblebee::AggregationType::SumAggregate:
        name = "sum";
        break;
      case bumblebee::AggregationType::MinAggregate:
        name = "min";
        break;
      case bumblebee::AggregationType::MaxAggregate:
        name = "max";
        break;
      default:
        name = "unknown";
        break;
    }
    return fmt::formatter<fmt::string_view>::format(name, ctx);
  }
};

namespace bumblebee {

/**
 * Groups its child's tuples by the group-by expressions and applies the aggregates
 * to each group.
 *
 * The output schema is the group-by columns followed by the aggregate columns.
 *
 * Note what is deliberately absent: bustub keeps `AggregateKey` / `AggregateValue`
 * structs next to this plan node, but those describe an executor's hash table, not
 * the plan. BumbleBee's engine will use its own PRLHashTable, so they have no place
 * here.
 */
class AggregationPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct an aggregation.
   *
   * @param output_schema The output schema: group-by columns, then aggregate columns.
   * @param child The child plan.
   * @param group_bys The group-by expressions. Empty for a whole-table aggregate.
   * @param aggregates The expressions being aggregated, one per aggregate.
   * @param agg_types The aggregate function for each of `aggregates`.
   */
  AggregationPlanNode(SchemaRef output_schema, AbstractPlanNodeRef child,
                      std::vector<AbstractExpressionRef> group_bys,
                      std::vector<AbstractExpressionRef> aggregates, std::vector<AggregationType> agg_types)
      : AbstractPlanNode(std::move(output_schema), {std::move(child)}),
        group_bys_(std::move(group_bys)),
        aggregates_(std::move(aggregates)),
        agg_types_(std::move(agg_types)) {}

  auto GetType() const -> PlanType override { return PlanType::Aggregation; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Aggregation should have exactly one child plan.");
    return GetChildAt(0);
  }

  /** @return The group-by expression at `idx`. */
  auto GetGroupByAt(uint32_t idx) const -> const AbstractExpressionRef & { return group_bys_[idx]; }

  /** @return The group-by expressions. */
  auto GetGroupBys() const -> const std::vector<AbstractExpressionRef> & { return group_bys_; }

  /** @return The aggregated expression at `idx`. */
  auto GetAggregateAt(uint32_t idx) const -> const AbstractExpressionRef & { return aggregates_[idx]; }

  /** @return The aggregated expressions. */
  auto GetAggregates() const -> const std::vector<AbstractExpressionRef> & { return aggregates_; }

  /** @return The aggregate function of each aggregated expression. */
  auto GetAggregateTypes() const -> const std::vector<AggregationType> & { return agg_types_; }

  /**
   * @brief The schema of an aggregation: the group-by columns, then the aggregate columns.
   *
   * @param group_bys The group-by expressions.
   * @param aggregates The aggregated expressions.
   * @param agg_types The aggregate functions.
   * @return Schema The output schema.
   */
  static auto InferAggSchema(const std::vector<AbstractExpressionRef> &group_bys,
                             const std::vector<AbstractExpressionRef> &aggregates,
                             const std::vector<AggregationType> &agg_types) -> Schema;

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(AggregationPlanNode);

  /** The group-by expressions. */
  std::vector<AbstractExpressionRef> group_bys_;
  /** The expressions being aggregated. */
  std::vector<AbstractExpressionRef> aggregates_;
  /** The aggregate function of each of `aggregates_`. */
  std::vector<AggregationType> agg_types_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
