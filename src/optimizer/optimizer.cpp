//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// optimizer.cpp
//
// Identification: src/optimizer/optimizer.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "optimizer/optimizer.h"

#include <optional>
#include <unordered_set>

#include "common/util/string_util.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

namespace {

/** @brief Add every node type in the tree to `present`. */
void CollectPlanTypes(const AbstractPlanNodeRef &plan, std::unordered_set<PlanType> &present) {
  present.insert(plan->GetType());
  for (const auto &child : plan->GetChildren()) {
    CollectPlanTypes(child, present);
  }
}

}  // namespace

auto Optimizer::Optimize(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  auto p = plan;

  // Every rewrite below walks the tree AND rebuilds it (each node is re-created to swap children in),
  // so a pass whose trigger node is not in the plan still costs a full clone. That is invisible on an
  // analytic query and expensive on the tiny statements a DML workload is made of — an
  // `INSERT ... VALUES` is Insert<-Projection<-Values, where seven of the nine passes can do nothing.
  // One cheap pre-walk collects which node types exist, and the guards below are deliberately COARSE:
  // a group of passes is skipped only when NONE of them could match anything.
  std::unordered_set<PlanType> present;
  CollectPlanTypes(p, present);
  const auto has = [&present](PlanType t) { return present.find(t) != present.end(); };
  const bool joins_or_filters = has(PlanType::Filter) || has(PlanType::NestedLoopJoin);
  const bool prunable =
      has(PlanType::SeqScan) || has(PlanType::HashJoin) || has(PlanType::NestedLoopJoin);

  // Order matters. MergeFilterNLJ must run before NLJAsHashJoin, because the
  // planner emits a cross product plus a separate Filter, and the join predicate
  // has to be folded into the join before we can look at it and decide it is an
  // equi-join. Likewise EliminateTrueFilter runs after MergeFilterNLJ so that the
  // cross product's always-true predicate is gone by the time we look for it.
  if (has(PlanType::Projection)) {
    p = OptimizeMergeProjection(p);
  }
  if (joins_or_filters) {
    // Cost-based join reordering, on the raw `Filter over cross-product chain` the planner emits (all
    // predicate refs in one flat frame). It reorders a 3+-table inner region into a bushy tree of
    // inner joins ONLY when the search finds a plan cheaper than the FROM order — using per-key NDV
    // derived from row counts (so a low-cardinality key like nationkey reads as near-cartesian, not
    // cheap). The passes below lower the emitted inner joins to hash joins exactly as before.
    p = OptimizeJoinOrder(p);
    p = OptimizeMergeFilterNLJ(p);
    // Between the two: splitting the join predicate is what lets NLJAsHashJoin fire on
    // a query that has both a join condition and a WHERE clause.
    p = OptimizeFilterPushDown(p);
    p = OptimizeNLJAsHashJoin(p);
    p = OptimizeEliminateTrueFilter(p);
    p = OptimizeMergeFilterScan(p);
  }
  if (has(PlanType::Limit) || has(PlanType::Sort)) {
    p = OptimizeSortLimitAsTopN(p);
  }
  if (prunable) {
    p = OptimizeColumnPruning(p);
  }
  return p;
}

auto Optimizer::EstimatedCardinality(const std::string &table_name) -> std::optional<size_t> {
  // Prefer a real row count from the table's storage: a heap sums its slotted-page tuple counts, a
  // parquet table sums its manifest's per-file counts. This is the base-cardinality signal the
  // cost-based join-order pass runs on.
  if (auto info = catalog_.GetTable(table_name); info != NULL_TABLE_INFO && info->storage_ != nullptr) {
    if (const auto rows = info->storage_->EstimatedRowCount(); rows > 0) {
      return std::make_optional(static_cast<size_t>(rows));
    }
  }
  // Fallback: the legacy name-suffix hint, still used by optimizer unit tests with synthetic names.
  if (StringUtil::EndsWith(table_name, "_1m")) {
    return std::make_optional(1000000);
  }
  if (StringUtil::EndsWith(table_name, "_100k")) {
    return std::make_optional(100000);
  }
  if (StringUtil::EndsWith(table_name, "_50k")) {
    return std::make_optional(50000);
  }
  if (StringUtil::EndsWith(table_name, "_10k")) {
    return std::make_optional(10000);
  }
  if (StringUtil::EndsWith(table_name, "_1k")) {
    return std::make_optional(1000);
  }
  if (StringUtil::EndsWith(table_name, "_100")) {
    return std::make_optional(100);
  }
  return std::nullopt;
}

}  // namespace bumblebee
