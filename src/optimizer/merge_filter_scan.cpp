//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// merge_filter_scan.cpp
//
// Identification: src/optimizer/merge_filter_scan.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <vector>

#include "common/macros.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

/**
 * @brief Push a Filter sitting directly on a SeqScan into the scan itself.
 *
 * The scan can then discard non-matching tuples as it reads them, instead of
 * handing every tuple to a separate operator that throws most of them away. This is
 * also the hook a future index-scan rule would match on.
 */
auto Optimizer::OptimizeMergeFilterScan(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeMergeFilterScan(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::Filter) {
    return optimized_plan;
  }

  const auto &filter_plan = dynamic_cast<const FilterPlanNode &>(*optimized_plan);
  BUMBLEBEE_ASSERT(optimized_plan->children_.size() == 1, "Filter should have exactly one child.");

  const auto &child_plan = *optimized_plan->children_[0];
  if (child_plan.GetType() != PlanType::SeqScan) {
    return optimized_plan;
  }

  const auto &seq_scan_plan = dynamic_cast<const SeqScanPlanNode &>(child_plan);
  // If the scan already carries a predicate, folding this one in would silently
  // drop it. Leave the filter where it is.
  if (seq_scan_plan.filter_predicate_ != nullptr) {
    return optimized_plan;
  }

  return std::make_shared<SeqScanPlanNode>(filter_plan.output_schema_, seq_scan_plan.table_oid_,
                                           seq_scan_plan.table_name_, filter_plan.GetPredicate());
}

}  // namespace bumblebee
