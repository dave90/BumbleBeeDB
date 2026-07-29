//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// greedy_join_order.h
//
// Identification: src/include/optimizer/join_order/greedy_join_order.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "optimizer/join_order/join_order_algorithm.h"

namespace bumblebee {

/**
 * @brief Greedy Operator Ordering (GOO): repeatedly join the pair of subtrees whose result is
 * estimated smallest, until one remains.
 *
 * `O(n³)`, produces bushy trees, and — because it ranks *all* pairs by estimated result size
 * (including selective cross products), not just equi-connected ones — it both avoids blind cross
 * products (their `|A|·|B|` cost loses) and discovers the good bushy plans a left-deep heuristic
 * cannot. Ties break by relation order, so the plan is deterministic.
 */
class GreedyJoinOrder : public JoinOrderAlgorithm {
 public:
  auto Order(const JoinGraph &graph, const CardinalityEstimator &estimator)
      -> std::unique_ptr<JoinTreeNode> override;

  auto Name() const -> std::string override { return "greedy"; }
};

}  // namespace bumblebee
