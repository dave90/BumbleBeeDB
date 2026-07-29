//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// greedy_join_order.cpp
//
// Identification: src/optimizer/join_order/greedy_join_order.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "optimizer/join_order/greedy_join_order.h"

#include <limits>
#include <utility>
#include <vector>

namespace bumblebee {

auto GreedyJoinOrder::Order(const JoinGraph &graph, const CardinalityEstimator &estimator)
    -> std::unique_ptr<JoinTreeNode> {
  // Working set: one leaf subtree per relation. Each iteration fuses the cheapest pair.
  std::vector<std::unique_ptr<JoinTreeNode>> subtrees;
  subtrees.reserve(graph.RelationCount());
  for (const auto &relation : graph.Relations()) {
    auto leaf = std::make_unique<JoinTreeNode>();
    leaf->relations_ = RelationSet::Singleton(relation.id_);
    leaf->cardinality_ = relation.cardinality_;
    leaf->leaf_id_ = relation.id_;
    subtrees.push_back(std::move(leaf));
  }
  if (subtrees.empty()) {
    return nullptr;
  }

  while (subtrees.size() > 1) {
    // Scan every pair; keep the one whose joined result is estimated smallest. Strict `<` keeps the
    // first-found pair on ties, so the order is deterministic.
    idx_t best_i = 0;
    idx_t best_j = 1;
    double best_card = std::numeric_limits<double>::max();
    for (idx_t i = 0; i < subtrees.size(); i++) {
      for (idx_t j = i + 1; j < subtrees.size(); j++) {
        const auto crossing = graph.EdgesCrossing(subtrees[i]->relations_, subtrees[j]->relations_);
        const double card = estimator.EstimateJoin(subtrees[i]->cardinality_, subtrees[j]->cardinality_, crossing);
        if (card < best_card) {
          best_card = card;
          best_i = i;
          best_j = j;
        }
      }
    }

    auto parent = std::make_unique<JoinTreeNode>();
    parent->relations_ = subtrees[best_i]->relations_.Union(subtrees[best_j]->relations_);
    parent->cardinality_ = best_card;
    parent->left_ = std::move(subtrees[best_i]);
    parent->right_ = std::move(subtrees[best_j]);

    // Erase the higher index first so the lower index stays valid.
    subtrees.erase(subtrees.begin() + static_cast<std::ptrdiff_t>(best_j));
    subtrees.erase(subtrees.begin() + static_cast<std::ptrdiff_t>(best_i));
    subtrees.push_back(std::move(parent));
  }

  return std::move(subtrees.front());
}

}  // namespace bumblebee
