//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// insert_plan.h
//
// Identification: src/include/execution/plans/insert_plan.h
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
 * Inserts every tuple its child produces into a table.
 */
class InsertPlanNode : public AbstractPlanNode {
 public:
  /**
   * @brief Construct an insert.
   *
   * @param output The output schema: a single column holding the number of rows inserted.
   * @param child The child producing the rows to insert.
   * @param table_oid The table to insert into.
   */
  InsertPlanNode(SchemaRef output, AbstractPlanNodeRef child, table_oid_t table_oid)
      : AbstractPlanNode(std::move(output), {std::move(child)}), table_oid_(table_oid) {}

  auto GetType() const -> PlanType override { return PlanType::Insert; }

  /** @return The table being inserted into. */
  auto GetTableOid() const -> table_oid_t { return table_oid_; }

  /** @return The child plan. */
  auto GetChildPlan() const -> AbstractPlanNodeRef {
    BUMBLEBEE_ASSERT(GetChildren().size() == 1, "Insert should have exactly one child plan.");
    return GetChildAt(0);
  }

  BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(InsertPlanNode);

  /** The table being inserted into. */
  table_oid_t table_oid_;

 protected:
  auto PlanNodeToString() const -> std::string override {
    return fmt::format("Insert {{ table_oid={} }}", table_oid_);
  }
};

}  // namespace bumblebee
