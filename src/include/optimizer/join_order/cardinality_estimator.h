//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// cardinality_estimator.h
//
// Identification: src/include/optimizer/join_order/cardinality_estimator.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <vector>

#include "optimizer/join_order/join_graph.h"

namespace bumblebee {

/**
 * @brief The cost oracle the join-order search consults: how big is `A ⋈ B`?
 *
 * System-R style, and deliberately size-only for now (no distinct-value counts yet — see the plan's
 * Phase 1b): the result of an equi-join is estimated as `|A|·|B| / max(V(A.x), V(B.y))`, and without
 * distinct-value counts the denominator is approximated by `min(|A|,|B|)` — the classic FK→PK
 * heuristic, where the smaller (dimension/PK) side bounds the distinct count. That gives a single
 * equi-join a result of `max(|A|,|B|)`: joining a big fact table to a small dimension keeps the fact
 * size (every FK row matches one PK row), NOT `min` — a subtle but decisive point (using `min` makes
 * a fact⋈dimension join look tiny and misorders the plan). A residual/non-equi predicate shrinks by a
 * fixed factor, and a bare cross product (no predicate) is the full `|A|·|B|` — which is how the
 * search learns to avoid cross products by cost rather than by rule. When distinct-value stats land,
 * only the equi denominator changes (to `max(NDV)`); everything else stays.
 */
class CardinalityEstimator {
 public:
  /** Selectivity of a single non-equi / residual conjunct when no better estimate is available. */
  static constexpr double RESIDUAL_SELECTIVITY = 0.1;

  /**
   * @brief Estimate the row count of joining two subtrees of sizes `left` and `right`, given the
   * predicates that first become evaluable at this join.
   *
   * @param left  Estimated rows on the left subtree.
   * @param right Estimated rows on the right subtree.
   * @param crossing The predicates connecting the two sides (empty ⇒ cross product).
   */
  auto EstimateJoin(double left, double right, const std::vector<const JoinEdge *> &crossing) const -> double {
    if (crossing.empty()) {
      return left * right;  // cross product: no predicate to shrink it
    }
    // Equi selectivity ≈ 1/min(|A|,|B|): the smaller side bounds the distinct count under FK→PK
    // containment, so a single equi-join yields ~max(|A|,|B|) (the fact-side size), not min.
    double selectivity = 1.0;
    for (const auto *edge : crossing) {
      if (!edge->is_equi_) {
        selectivity *= RESIDUAL_SELECTIVITY;
      } else if (edge->ndv_ > 0) {
        // Accurate equi selectivity: 1/NDV of the join key. A low-cardinality key (nationkey ≈ 25)
        // barely shrinks the cross product → the estimator prices the join as near-cartesian.
        selectivity *= 1.0 / edge->ndv_;
      } else {
        // No key stats: fall back to 1/min(size) (FK→PK — result ≈ max(|A|,|B|)).
        const double denom = std::min(left, right);
        selectivity *= denom > 0 ? 1.0 / denom : 1.0;
      }
    }
    return std::max(1.0, left * right * selectivity);
  }
};

}  // namespace bumblebee
