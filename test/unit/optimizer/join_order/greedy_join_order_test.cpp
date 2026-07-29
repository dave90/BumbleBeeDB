//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// greedy_join_order_test.cpp
//
// Identification: test/unit/optimizer/join_order/greedy_join_order_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "optimizer/join_order/greedy_join_order.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "optimizer/join_order/cardinality_estimator.h"
#include "optimizer/join_order/join_graph.h"

namespace bumblebee {

namespace {

/** @brief Add a relation of a given estimated size; returns its id. */
auto AddRel(JoinGraph &g, double card) -> idx_t {
  JoinRelation r;
  r.cardinality_ = card;
  return g.AddRelation(r);
}

/** @brief Collect the relation set of every INTERNAL node of the tree (leaves excluded). */
void CollectInternalSets(const JoinTreeNode *node, std::vector<RelationSet> &out) {
  if (node == nullptr || node->IsLeaf()) {
    return;
  }
  out.push_back(node->relations_);
  CollectInternalSets(node->left_.get(), out);
  CollectInternalSets(node->right_.get(), out);
}

auto HasInternalSet(const JoinTreeNode *root, RelationSet target) -> bool {
  std::vector<RelationSet> sets;
  CollectInternalSets(root, sets);
  for (const auto s : sets) {
    if (s == target) {
      return true;
    }
  }
  return false;
}

}  // namespace

// q02 shape: part—partsupp—supplier—nation—region, a chain. part and supplier share NO edge (they
// connect only through partsupp), so a left-deep FROM-order join would cross-product them. GOO must
// NOT: it should never create an internal node that is exactly {part, supplier}.
TEST(GreedyJoinOrderTest, AvoidsCrossProductOnQ02Chain) {
  JoinGraph g;
  const auto part = AddRel(g, 200);        // filtered p_size=15 AND p_type LIKE '%BRASS'
  const auto partsupp = AddRel(g, 800000);
  const auto supplier = AddRel(g, 10000);
  const auto nation = AddRel(g, 5);        // filtered to EUROPE region
  const auto region = AddRel(g, 1);

  g.AddEdge(RelationSet::Singleton(part).Union(RelationSet::Singleton(partsupp)), nullptr, /*is_equi=*/true);
  g.AddEdge(RelationSet::Singleton(supplier).Union(RelationSet::Singleton(partsupp)), nullptr, true);
  g.AddEdge(RelationSet::Singleton(supplier).Union(RelationSet::Singleton(nation)), nullptr, true);
  g.AddEdge(RelationSet::Singleton(nation).Union(RelationSet::Singleton(region)), nullptr, true);

  CardinalityEstimator est;
  GreedyJoinOrder goo;
  auto tree = goo.Order(g, est);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(tree->relations_.Count(), 5U);  // covers all relations

  const auto part_supplier = RelationSet::Singleton(part).Union(RelationSet::Singleton(supplier));
  EXPECT_FALSE(HasInternalSet(tree.get(), part_supplier)) << "GOO formed the part×supplier cross product";
}

// mq07 shape: a fact chain (supplier—lineitem—orders—customer) with two dimension copies n1, n2 that
// have NO equi edge to each other — only the OR filter linking them. GOO must join {n1, n2} FIRST
// (625 rows ≪ anything touching the 30M fact stream), which is only possible because the OR is an
// edge in the graph.
TEST(GreedyJoinOrderTest, JoinsTheTwoNationsFirstOnMq07) {
  JoinGraph g;
  const auto supplier = AddRel(g, 10000);
  const auto lineitem = AddRel(g, 30000000);
  const auto orders = AddRel(g, 7500000);
  const auto customer = AddRel(g, 1500000);
  const auto n1 = AddRel(g, 25);
  const auto n2 = AddRel(g, 25);

  g.AddEdge(RelationSet::Singleton(supplier).Union(RelationSet::Singleton(lineitem)), nullptr, true);
  g.AddEdge(RelationSet::Singleton(lineitem).Union(RelationSet::Singleton(orders)), nullptr, true);
  g.AddEdge(RelationSet::Singleton(orders).Union(RelationSet::Singleton(customer)), nullptr, true);
  g.AddEdge(RelationSet::Singleton(supplier).Union(RelationSet::Singleton(n1)), nullptr, true);
  g.AddEdge(RelationSet::Singleton(customer).Union(RelationSet::Singleton(n2)), nullptr, true);
  // The OR that links the two nations — a NON-equi edge, but an edge nonetheless.
  g.AddEdge(RelationSet::Singleton(n1).Union(RelationSet::Singleton(n2)), nullptr, /*is_equi=*/false);

  CardinalityEstimator est;
  GreedyJoinOrder goo;
  auto tree = goo.Order(g, est);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(tree->relations_.Count(), 6U);

  const auto pair = RelationSet::Singleton(n1).Union(RelationSet::Singleton(n2));
  EXPECT_TRUE(HasInternalSet(tree.get(), pair)) << "GOO did not pre-join the two nations";
  // And it never joined a nation straight onto the fact stream instead.
  EXPECT_FALSE(HasInternalSet(tree.get(), RelationSet::Singleton(lineitem).Union(RelationSet::Singleton(n1))));
}

// With per-key NDV, GOO must AVOID joining on a low-cardinality key early. A q05-shaped graph where
// `nationkey` has NDV 25 (near-cartesian) but the fact keys (custkey/orderkey/suppkey) are unique:
// GOO must connect the fact tables on the high-NDV keys before pulling in the nation join.
TEST(GreedyJoinOrderTest, AvoidsLowCardinalityKeyJoinWithNdv) {
  JoinGraph g;
  const auto customer = AddRel(g, 1500000);
  const auto orders = AddRel(g, 7500000);
  const auto lineitem = AddRel(g, 30000000);
  const auto supplier = AddRel(g, 10000);
  const auto nation = AddRel(g, 25);

  auto pair = [](idx_t x, idx_t y) { return RelationSet::Singleton(x).Union(RelationSet::Singleton(y)); };
  // High-NDV fact keys (unique on the PK side).
  g.AddEdge(pair(customer, orders), nullptr, /*is_equi=*/true, /*ndv=*/1500000);   // c_custkey = o_custkey
  g.AddEdge(pair(orders, lineitem), nullptr, true, 7500000);                       // o_orderkey = l_orderkey
  g.AddEdge(pair(supplier, lineitem), nullptr, true, 10000);                       // s_suppkey = l_suppkey
  // Low-cardinality nationkey joins (25 distinct) — joining on these is near-cartesian.
  g.AddEdge(pair(customer, supplier), nullptr, true, 25);                          // c_nationkey = s_nationkey
  g.AddEdge(pair(supplier, nation), nullptr, true, 25);                            // s_nationkey = n_nationkey

  CardinalityEstimator est;
  GreedyJoinOrder goo;
  auto tree = goo.Order(g, est);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(tree->relations_.Count(), 5U);
  // The catastrophic join is customer⋈supplier on nationkey (1.5M × 10k / 25 ≈ 600M). GOO must never
  // form exactly {customer, supplier} — it should reach both only through the fact tables.
  EXPECT_FALSE(HasInternalSet(tree.get(), pair(customer, supplier)))
      << "GOO joined customer and supplier directly on the low-cardinality nationkey";
}

// A single relation returns a bare leaf; an empty graph returns null.
TEST(GreedyJoinOrderTest, DegenerateGraphs) {
  CardinalityEstimator est;
  GreedyJoinOrder goo;

  JoinGraph empty;
  EXPECT_EQ(goo.Order(empty, est), nullptr);

  JoinGraph one;
  AddRel(one, 42);
  auto tree = goo.Order(one, est);
  ASSERT_NE(tree, nullptr);
  EXPECT_TRUE(tree->IsLeaf());
  EXPECT_EQ(tree->cardinality_, 42.0);
}

}  // namespace bumblebee
