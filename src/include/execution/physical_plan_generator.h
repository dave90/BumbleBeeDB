//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_plan_generator.h
//
// Identification: src/include/execution/physical_plan_generator.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <unordered_set>

#include "execution/physical_operator.h"
#include "storage/table/table_storage.h"
#include "execution/plans/abstract_plan.h"
#include "main/client_context.h"

namespace bumblebee {

/**
 * @brief Lowers a logical `AbstractPlanNode` tree into a physical `PhysicalOperator` tree by
 * pattern-matching on `PlanType` — the milestone the logical plan was deliberately kept strategy-free
 * for. One private method per `PlanType`.
 */
class PhysicalPlanGenerator {
 public:
  explicit PhysicalPlanGenerator(ClientContext &context) : context_(context) {}

  /** @brief Lower `plan` into a physical operator tree (no root collector — the driver adds that). */
  auto CreatePlan(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;

  /** @brief Lower `plan` and, if it is a SELECT, wrap it in a `PhysicalResultCollector`. */
  auto PlanRoot(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;

  /** @return The storage format behind `oid` (dispatches scan/write lowering per backend). */
  auto TableStorageFormat(table_oid_t oid) const -> StorageFormat;

  /** @brief Force these logical nodes to lower to their external, memory-bounded variants on retry. */
  void SetForceExternal(const std::unordered_set<const AbstractPlanNode *> &nodes) { force_external_ = nodes; }

 private:
  auto CreateSeqScan(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateFilter(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateProjection(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateValues(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateInsert(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateDelete(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateUpdate(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateAggregation(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateHashJoin(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateNestedLoopJoin(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateSort(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateLimit(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  auto CreateTopN(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
  /** @brief Lower a DML source (scan/filter) so the scan appends a RID column; sets `rid_column`. */
  auto LowerDmlChild(const AbstractPlanNodeRef &child, idx_t &rid_column) -> std::unique_ptr<PhysicalOperator>;

  /** @brief True if `plan` must lower to its external variant (forced by config or a prior retry). */
  auto UseExternal(const AbstractPlanNodeRef &plan) const -> bool {
    // TODO(planner): statistics-based selection. Today the choice is binary and reactive — default to
    // in-memory, and only go external when the config forces it or a prior attempt overflowed.
    // Once the catalog carries table/column statistics (row counts, NDV, avg row width), estimate the
    // build/sort footprint here (est_rows * est_row_bytes) and choose external UP FRONT when it clearly
    // exceeds the query memory budget — avoiding the abort-and-retry round trip for inputs we already
    // know won't fit. Keep the runtime overflow path as the safety net for when the estimate is wrong.
    return context_.config_.prefer_external_ || force_external_.count(plan.get()) != 0;
  }

  ClientContext &context_;
  std::unordered_set<const AbstractPlanNode *> force_external_;
};

}  // namespace bumblebee
