//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// delete_plan.h
//
// Identification: src/include/execution/plans/delete_plan.h
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
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/**
 * Deletes every tuple its child produces from a table.
 */
class DeletePlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct a delete.
   *
   * @param output The output schema: a single column holding the number of rows deleted.
   * @param child The child producing the rows to delete.
   * @param table_oid The table to delete from.
   */
  DeletePlanNode(SchemaRef output, AbstractPlanNodeRef child, table_oid_t table_oid)
      : AbstractPlanNode(std::move(output), {std::move(child)}), table_oid_{table_oid} {}

  auto GetType() const -> PlanType override { return PlanType::Delete; }

  /** @return The table being deleted from. */
  auto GetTableOid() const -> table_oid_t { return table_oid_; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Delete should have exactly one child plan.");
    return GetChildAt(0);
  }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(DeletePlanNode);

  /** The table being deleted from. */
  table_oid_t table_oid_;

 protected:
  auto PlanNodeToString() const -> std::string override {
    return fmt::format("Delete {{ table_oid={} }}", table_oid_);
  }
};

}  // namespace bumblebee
