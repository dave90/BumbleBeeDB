//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// join_graph.h
//
// Identification: src/include/optimizer/join_order/join_graph.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <vector>

#include "common/config.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/abstract_plan.h"

namespace bumblebee {

/** The largest inner-join region the cost-based reorderer will consider (a `RelationSet` is a 64-bit
 * mask, and enumeration stays cheap well past this; above it the caller keeps the left-deep plan). */
constexpr idx_t MAX_REORDER_RELATIONS = 12;

/**
 * @brief A set of base relations, as a bitmask over relation ids (`0..MAX_REORDER_RELATIONS-1`).
 *
 * The currency of join enumeration: a subtree "covers" a `RelationSet`, a predicate "references" one,
 * and a join combines two disjoint sets. Cheap value type — copy freely.
 */
struct RelationSet {
  uint64_t bits_{0};

  static auto Singleton(idx_t id) -> RelationSet { return RelationSet{uint64_t{1} << id}; }

  auto Union(RelationSet other) const -> RelationSet { return RelationSet{bits_ | other.bits_}; }
  auto Intersects(RelationSet other) const -> bool { return (bits_ & other.bits_) != 0; }
  auto IsSubsetOf(RelationSet other) const -> bool { return (bits_ & ~other.bits_) == 0; }
  auto Contains(idx_t id) const -> bool { return (bits_ & (uint64_t{1} << id)) != 0; }
  auto Count() const -> idx_t { return static_cast<idx_t>(__builtin_popcountll(bits_)); }
  auto Empty() const -> bool { return bits_ == 0; }
  auto operator==(RelationSet other) const -> bool { return bits_ == other.bits_; }
};

/**
 * @brief One leaf of a reordered region: a base relation with its estimated size and its column
 * range in the region's flat concatenated schema (the offset the emitter re-indexes against).
 */
struct JoinRelation {
  idx_t id_{0};
  double cardinality_{1};
  AbstractPlanNodeRef plan_;  // the leaf subplan (may be null in isolated unit tests)
  idx_t base_col_offset_{0};  // this relation's first column in the region's flat schema
  idx_t col_count_{0};
};

/**
 * @brief One predicate connecting relations. Equi predicates (`col = col`) drive both selectivity and
 * later the hash-join keys; non-equi predicates (a residual, or the OR that links two dimension
 * tables) are edges too — that is what lets the search consider joining them.
 */
struct JoinEdge {
  RelationSet relations_;  // every base relation this predicate references
  AbstractExpressionRef predicate_;
  bool is_equi_{false};
  /** For an equi edge: the join key's distinct-value count, approximated as the smallest row count
   * among the tables joined transitively on that key (0 = unknown → size-only fallback). This is what
   * lets the estimator see a low-cardinality key (e.g. nationkey ≈ 25) as a near-cross-product. */
  double ndv_{0};

  /** @return True if this predicate first becomes evaluable at the join of `a` and `b`: it references
   * relations on both sides and none outside their union. */
  auto Crosses(RelationSet a, RelationSet b) const -> bool {
    return relations_.IsSubsetOf(a.Union(b)) && relations_.Intersects(a) && relations_.Intersects(b);
  }
};

/** @brief The query graph over a maximal inner-join region: relations (nodes) and predicates (edges). */
class JoinGraph {
 public:
  auto AddRelation(JoinRelation relation) -> idx_t {
    const auto id = relations_.size();
    relation.id_ = id;
    relations_.push_back(std::move(relation));
    return id;
  }

  void AddEdge(RelationSet relations, AbstractExpressionRef predicate, bool is_equi, double ndv = 0) {
    edges_.push_back(JoinEdge{relations, std::move(predicate), is_equi, ndv});
  }

  auto Relations() const -> const std::vector<JoinRelation> & { return relations_; }

  auto RelationCount() const -> idx_t { return relations_.size(); }

  /** @return Every edge that first becomes evaluable at the join of `a` and `b`. */
  auto EdgesCrossing(RelationSet a, RelationSet b) const -> std::vector<const JoinEdge *> {
    std::vector<const JoinEdge *> crossing;
    for (const auto &edge : edges_) {
      if (edge.Crosses(a, b)) {
        crossing.push_back(&edge);
      }
    }
    return crossing;
  }

 private:
  std::vector<JoinRelation> relations_;
  std::vector<JoinEdge> edges_;
};

}  // namespace bumblebee
