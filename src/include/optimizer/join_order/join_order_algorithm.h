//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// join_order_algorithm.h
//
// Identification: src/include/optimizer/join_order/join_order_algorithm.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "optimizer/join_order/cardinality_estimator.h"
#include "optimizer/join_order/join_graph.h"

namespace bumblebee {

/**
 * @brief A join *shape*: a binary tree over the region's relations. Leaves are single relations; an
 * internal node joins its two children. This is the output of a `JoinOrderAlgorithm` — the emitter
 * turns it into physical hash/nested-loop joins with the actual predicates and build-side choices.
 */
struct JoinTreeNode {
  RelationSet relations_;                    // the base relations this subtree covers
  double cardinality_{1};                    // estimated rows out of this subtree
  idx_t leaf_id_{0};                         // meaningful only for a leaf
  std::unique_ptr<JoinTreeNode> left_;       // null ⇔ leaf
  std::unique_ptr<JoinTreeNode> right_;

  auto IsLeaf() const -> bool { return left_ == nullptr; }
};

/**
 * @brief The swap point: choose a join order over a `JoinGraph`, given a cost oracle.
 *
 * Narrow on purpose — in = graph + estimator, out = a join shape. Concrete algorithms
 * (`GreedyJoinOrder` now; a DPccp enumerator later) implement only the combinatorial search; they
 * touch no expressions, no column re-indexing, and no physical operators (all shared, in the emitter).
 */
class JoinOrderAlgorithm {
 public:
  virtual ~JoinOrderAlgorithm() = default;

  /** @brief Produce a join tree over `graph`. Returns null for an empty graph. */
  virtual auto Order(const JoinGraph &graph, const CardinalityEstimator &estimator)
      -> std::unique_ptr<JoinTreeNode> = 0;

  /** @brief A short name for EXPLAIN / logging. */
  virtual auto Name() const -> std::string = 0;
};

}  // namespace bumblebee
