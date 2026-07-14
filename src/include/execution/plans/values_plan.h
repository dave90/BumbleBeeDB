//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// values_plan.h
//
// Identification: src/include/execution/plans/values_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * A literal set of rows, as in `INSERT INTO t VALUES (1, 2), (3, 4)`. A leaf.
 */
class ValuesPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a set of literal rows.
   *
   * @param output The output schema.
   * @param values The rows, each a list of one expression per column.
   */
  explicit ValuesPlanNode(SchemaRef output, std::vector<std::vector<AbstractExpressionRef>> values)
      : AbstractPlanNode(std::move(output), {}), values_(std::move(values)) {}

  auto GetType() const -> PlanType override { return PlanType::Values; }

  /** @return The rows. */
  auto GetValues() const -> const std::vector<std::vector<AbstractExpressionRef>> & { return values_; }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(ValuesPlanNode);

  /** The rows, each a list of one expression per column. */
  std::vector<std::vector<AbstractExpressionRef>> values_;

 protected:
  auto PlanNodeToString() const -> std::string override {
    return fmt::format("Values {{ rows={} }}", values_.size());
  }
};

}  // namespace bumblebee
