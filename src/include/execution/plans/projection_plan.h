//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// projection_plan.h
//
// Identification: src/include/execution/plans/projection_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/schema.h"
#include "common/macros.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Evaluates a list of expressions over each tuple of its child.
 */
class ProjectionPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a projection.
   *
   * @param output The output schema.
   * @param expressions One expression per output column.
   * @param child The child plan.
   */
  ProjectionPlanNode(SchemaRef output, std::vector<AbstractExpressionRef> expressions, AbstractPlanNodeRef child)
      : AbstractPlanNode(std::move(output), {std::move(child)}), expressions_(std::move(expressions)) {}

  auto GetType() const -> PlanType override { return PlanType::Projection; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Projection should have exactly one child plan.");
    return GetChildAt(0);
  }

  /** @return One expression per output column. */
  auto GetExpressions() const -> const std::vector<AbstractExpressionRef> & { return expressions_; }

  /**
   * @brief The schema produced by evaluating `expressions`.
   *
   * @param expressions The projection expressions.
   * @return Schema The output schema.
   */
  static auto InferProjectionSchema(const std::vector<AbstractExpressionRef> &expressions) -> Schema;

  /**
   * @brief Copy `schema` with its columns renamed to `col_names`.
   *
   * @param schema The schema to rename.
   * @param col_names The new column names, one per column.
   * @return Schema The renamed schema.
   */
  static auto RenameSchema(const Schema &schema, const std::vector<std::string> &col_names) -> Schema;

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(ProjectionPlanNode);

  /** One expression per output column. */
  std::vector<AbstractExpressionRef> expressions_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
