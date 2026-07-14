//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// update_plan.h
//
// Identification: src/include/execution/plans/update_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/macros.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Rewrites every tuple its child produces and stores it back into a table.
 */
class UpdatePlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct an update.
   *
   * @param output The output schema: a single column holding the number of rows updated.
   * @param child The child producing the rows to update.
   * @param table_oid The table to update.
   * @param target_expressions One expression per table column, giving that column's
   *        new value. Columns not named in the SET clause get a plain column
   *        reference to their old value.
   */
  UpdatePlanNode(SchemaRef output, AbstractPlanNodeRef child, table_oid_t table_oid,
                 std::vector<AbstractExpressionRef> target_expressions)
      : AbstractPlanNode(std::move(output), {std::move(child)}),
        table_oid_{table_oid},
        target_expressions_(std::move(target_expressions)) {}

  auto GetType() const -> PlanType override { return PlanType::Update; }

  /** @return The table being updated. */
  auto GetTableOid() const -> table_oid_t { return table_oid_; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Update should have exactly one child plan.");
    return GetChildAt(0);
  }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(UpdatePlanNode);

  /** The table being updated. */
  table_oid_t table_oid_;

  /** One expression per table column, giving that column's new value. */
  std::vector<AbstractExpressionRef> target_expressions_;

 protected:
  auto PlanNodeToString() const -> std::string override;
};

}  // namespace bumblebee
