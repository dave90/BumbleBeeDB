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

#include "common/util/string_util.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

auto Optimizer::Optimize(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  auto p = plan;
  // Order matters. MergeFilterNLJ must run before NLJAsHashJoin, because the
  // planner emits a cross product plus a separate Filter, and the join predicate
  // has to be folded into the join before we can look at it and decide it is an
  // equi-join. Likewise EliminateTrueFilter runs after MergeFilterNLJ so that the
  // cross product's always-true predicate is gone by the time we look for it.
  p = OptimizeMergeProjection(p);
  p = OptimizeMergeFilterNLJ(p);
  // Between the two: splitting the join predicate is what lets NLJAsHashJoin fire on
  // a query that has both a join condition and a WHERE clause.
  p = OptimizeFilterPushDown(p);
  p = OptimizeNLJAsHashJoin(p);
  p = OptimizeEliminateTrueFilter(p);
  p = OptimizeMergeFilterScan(p);
  p = OptimizeSortLimitAsTopN(p);
  p = OptimizeColumnPruning(p);
  return p;
}

auto Optimizer::EstimatedCardinality(const std::string &table_name) -> std::optional<size_t> {
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
