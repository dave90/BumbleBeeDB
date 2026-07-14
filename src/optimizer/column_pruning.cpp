//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column_pruning.cpp
//
// Identification: src/optimizer/column_pruning.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "optimizer/optimizer.h"

namespace bumblebee {

/**
 * @brief Drop columns that no operator above ever reads. NOT IMPLEMENTED YET.
 *
 * This is a real win for a vectorized engine — every pruned column is a whole
 * vector that never has to be materialized or copied — and it is on the roadmap for
 * the execution milestone.
 *
 * It is deliberately not implemented now. Pruning a column means renumbering every
 * column reference above the pruned node, threading the change through Projection,
 * Aggregation and both join flavours. Getting that renumbering subtly wrong produces
 * a plan that still looks plausible and still runs, but silently reads the wrong
 * column — and with no execution engine yet, there is no test that could catch it.
 * It lands together with the engine, where result-equality can validate it.
 */
auto Optimizer::OptimizeColumnPruning(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef { return plan; }

}  // namespace bumblebee
