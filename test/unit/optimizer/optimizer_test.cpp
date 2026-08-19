//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// optimizer_test.cpp
//
// Identification: test/unit/optimizer/optimizer_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <functional>

#include "optimizer/optimizer.h"

#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/topn_plan.h"
#include "frontend_test_util.h"
#include "gtest/gtest.h"
#include "planner/planner.h"

namespace bumblebee {

/** @brief Bind, plan and optimize a query. */
static auto TryOptimize(const Catalog &catalog, const std::string &query) -> AbstractPlanNodeRef {
  auto statements = TryBind(catalog, query);
  EXPECT_EQ(statements.size(), 1U);
  Planner planner(catalog);
  planner.PlanQuery(*statements[0]);
  Optimizer optimizer(catalog);
  return optimizer.Optimize(planner.plan_);
}

/** @brief Bind and plan a query, without optimizing it. */
static auto TryPlan(const Catalog &catalog, const std::string &query) -> AbstractPlanNodeRef {
  auto statements = TryBind(catalog, query);
  Planner planner(catalog);
  planner.PlanQuery(*statements[0]);
  return planner.plan_;
}

// ---------------------------------------------------------------------------
// MergeProjection
// ---------------------------------------------------------------------------

TEST(OptimizerTest, MergeProjectionRemovesIdentityProjection) {
  auto catalog = MakeTestCatalog();
  // `SELECT *` plans as a projection that re-emits every column unchanged.
  EXPECT_EQ(CountPlanNodes(TryPlan(*catalog, "SELECT * FROM y"), PlanType::Projection), 1U);

  auto plan = TryOptimize(*catalog, "SELECT * FROM y");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Projection), 0U);
  EXPECT_EQ(plan->GetType(), PlanType::SeqScan);
  // The projection is gone but its schema — which carries the column names — remains.
  EXPECT_EQ(plan->OutputSchema().GetColumnCount(), 5U);
}

TEST(OptimizerTest, MergeProjectionKeepsARealProjection) {
  auto catalog = MakeTestCatalog();
  // This one reorders columns, so it is not an identity.
  auto plan = TryOptimize(*catalog, "SELECT z, x FROM y");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Projection), 1U);
}

// ---------------------------------------------------------------------------
// EliminateTrueFilter / MergeFilterNLJ
// ---------------------------------------------------------------------------

TEST(OptimizerTest, MergeFilterNLJ) {
  auto catalog = MakeTestCatalog();
  // The planner emits a cross product plus a separate Filter.
  auto unoptimized = TryPlan(*catalog, "SELECT * FROM a, b WHERE a.x = b.y");
  EXPECT_EQ(CountPlanNodes(unoptimized, PlanType::Filter), 1U);

  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.y");
  // The filter has been folded into the join, so no Filter node survives.
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);
}

TEST(OptimizerTest, EliminateTrueFilter) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b");
  // A bare cross product: the always-true predicate must not survive as a Filter.
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 1U);
}

// ---------------------------------------------------------------------------
// NLJAsHashJoin
// ---------------------------------------------------------------------------

TEST(OptimizerTest, NLJAsHashJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.y");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 0U);

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  ASSERT_EQ(hash_join.LeftJoinKeyExpressions().size(), 1U);
  ASSERT_EQ(hash_join.RightJoinKeyExpressions().size(), 1U);
  // The left key addresses input 0 and the right key input 1.
  EXPECT_EQ(hash_join.LeftJoinKeyExpressions()[0]->ToString(), "#0.0");
  EXPECT_EQ(hash_join.RightJoinKeyExpressions()[0]->ToString(), "#1.1");
}

TEST(OptimizerTest, NLJAsHashJoinMultipleKeys) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.x AND a.y = b.y");
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.LeftJoinKeyExpressions().size(), 2U);
  EXPECT_EQ(hash_join.RightJoinKeyExpressions().size(), 2U);
}

TEST(OptimizerTest, NLJAsHashJoinNormalizesReversedKeys) {
  auto catalog = MakeTestCatalog();
  // Written the other way round: `b.y = a.x`. The left key must still be the one
  // from the left input.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE b.y = a.x");
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.LeftJoinKeyExpressions()[0]->ToString(), "#0.0");
  EXPECT_EQ(hash_join.RightJoinKeyExpressions()[0]->ToString(), "#1.1");
}

TEST(OptimizerTest, DisjunctiveJoinStaysNestedLoop) {
  auto catalog = MakeTestCatalog();
  // A single hash table cannot answer an OR, so this must remain a nested loop join.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.x OR a.y = b.y");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 0U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 1U);
}

TEST(OptimizerTest, NonEquiJoinStaysNestedLoop) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x > b.x");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 0U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 1U);
}

// ---------------------------------------------------------------------------
// MergeFilterScan
// ---------------------------------------------------------------------------

TEST(OptimizerTest, MergeFilterScan) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM y WHERE x = 1");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);

  auto scan = FindPlanNode(plan, PlanType::SeqScan);
  ASSERT_NE(scan, nullptr);
  const auto &seq_scan = dynamic_cast<const SeqScanPlanNode &>(*scan);
  ASSERT_NE(seq_scan.filter_predicate_, nullptr);
  EXPECT_EQ(seq_scan.filter_predicate_->ToString(), "(#0.0=1)");
}

// ---------------------------------------------------------------------------
// SortLimitAsTopN
// ---------------------------------------------------------------------------

TEST(OptimizerTest, SortLimitAsTopN) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM y ORDER BY x LIMIT 10");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::TopN), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Sort), 0U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Limit), 0U);

  auto topn = FindPlanNode(plan, PlanType::TopN);
  ASSERT_NE(topn, nullptr);
  const auto &topn_plan = dynamic_cast<const TopNPlanNode &>(*topn);
  EXPECT_EQ(topn_plan.GetN(), 10U);
  EXPECT_EQ(topn_plan.GetOrderBy().size(), 1U);
}

TEST(OptimizerTest, SortWithoutLimitStaysASort) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM y ORDER BY x");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Sort), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::TopN), 0U);
}

TEST(OptimizerTest, LimitWithoutSortStaysALimit) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM y LIMIT 10");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Limit), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::TopN), 0U);
}

// ---------------------------------------------------------------------------
// FilterPushDown — and the composite query it exists to make work
// ---------------------------------------------------------------------------

TEST(OptimizerTest, FilterPushDownSplitsAMixedJoinPredicate) {
  auto catalog = MakeTestCatalog();
  // The crux: `a.x = b.x` is a join key but `a.y > 10` is a single-table filter.
  // Without pushdown, NLJAsHashJoin sees a predicate it cannot fully turn into keys
  // and gives up, leaving a quadratic nested loop join.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.x AND a.y > 10");

  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 0U);
  // The single-table conjunct ends up in the scan of `a`, not on the join.
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &left_scan = dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0));
  ASSERT_NE(left_scan.filter_predicate_, nullptr);
  // Re-based: inside the join it was `#0.1`, below the join it addresses `a` directly.
  EXPECT_EQ(left_scan.filter_predicate_->ToString(), "(#0.1>10)");

  // The right side has no predicate of its own.
  const auto &right_scan = dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1));
  EXPECT_EQ(right_scan.filter_predicate_, nullptr);
}

TEST(OptimizerTest, FilterPushDownPushesToBothSides) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.x AND a.y > 10 AND b.y < 5");
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  EXPECT_NE(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0)).filter_predicate_, nullptr);
  EXPECT_NE(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1)).filter_predicate_, nullptr);
}

TEST(OptimizerTest, FilterPushDownWorksThroughExplicitInnerJoinOn) {
  auto catalog = MakeTestCatalog();
  // Same intent as the comma-join case, but written as `INNER JOIN ... ON`. The ON
  // clause leaves the join with a live predicate, so MergeFilterNLJ must conjoin the
  // WHERE filter into it (rather than bailing) for pushdown to then split it out.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a JOIN b ON a.x = b.x WHERE a.y > 10");

  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 0U);
  // The filter reaches the scan of `a` — no stray Filter node is left above the join.
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &left_scan = dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0));
  ASSERT_NE(left_scan.filter_predicate_, nullptr);
  EXPECT_EQ(left_scan.filter_predicate_->ToString(), "(#0.1>10)");
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1)).filter_predicate_, nullptr);
}

TEST(OptimizerTest, FilterPushDownPushesBothSidesThroughInnerJoinOn) {
  auto catalog = MakeTestCatalog();
  // Two single-table conjuncts in the WHERE, one per side, on top of an explicit ON key.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a JOIN b ON a.x = b.x WHERE a.y > 10 AND b.y < 5");

  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &left_scan = dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0));
  const auto &right_scan = dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1));
  ASSERT_NE(left_scan.filter_predicate_, nullptr);
  ASSERT_NE(right_scan.filter_predicate_, nullptr);
  EXPECT_EQ(left_scan.filter_predicate_->ToString(), "(#0.1>10)");
  EXPECT_EQ(right_scan.filter_predicate_->ToString(), "(#0.1<5)");
}

TEST(OptimizerTest, FilterPushDownLeavesRightJoinWhereFilterAlone) {
  auto catalog = MakeTestCatalog();
  // RIGHT join is likewise non-preserving on the left; folding a WHERE filter in
  // would change the answer, so the Filter must survive above the join.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a RIGHT JOIN b ON a.x = b.x WHERE b.y > 10");

  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 1U);
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0)).filter_predicate_, nullptr);
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1)).filter_predicate_, nullptr);
}

TEST(OptimizerTest, FilterPushDownLeavesOuterJoinWhereFilterAlone) {
  auto catalog = MakeTestCatalog();
  // A WHERE filter on a LEFT join runs after null-padding, so it is NOT equivalent to
  // an ON predicate and must not be folded into (nor pushed below) the join.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a LEFT JOIN b ON a.x = b.x WHERE a.y > 10");

  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 1U);
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  // Neither scan absorbs the filter — it stays above the join.
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0)).filter_predicate_, nullptr);
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1)).filter_predicate_, nullptr);
}

TEST(OptimizerTest, FilterPushDownTurnsEveryLevelOfAMultiTableJoinIntoAHashJoin) {
  auto catalog = MakeTestCatalog();
  // The regression this rule exists to prevent: the planner nests the three tables as
  // (a JOIN b) JOIN d cross products, and MergeFilterNLJ folds the WHERE only into the
  // outermost one — so without recursive pushdown the inner (a, b) join stays a full
  // cross product with a Filter on top. Every level must become a hash join instead.
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b, d WHERE a.x = b.x AND b.y = d.y AND a.y > 10");

  // Every level is a hash join and no cross product survives — the invariant this guards. (The
  // cost-based join-order pass now picks the bushy shape and tops the region with a projection that
  // restores column order, so we assert the invariant rather than a fixed left-deep shape.)
  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 2U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 0U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 0U);

  // The single-table `a.y > 10` still reaches a scan, wherever `a` ends up in the reordered tree.
  std::function<const SeqScanPlanNode *(const AbstractPlanNodeRef &)> find_filtered_scan =
      [&](const AbstractPlanNodeRef &n) -> const SeqScanPlanNode * {
    if (n->GetType() == PlanType::SeqScan) {
      const auto &scan = dynamic_cast<const SeqScanPlanNode &>(*n);
      if (scan.filter_predicate_ != nullptr && scan.filter_predicate_->ToString() == "(#0.1>10)") {
        return &scan;
      }
    }
    for (const auto &child : n->GetChildren()) {
      if (const auto *found = find_filtered_scan(child); found != nullptr) {
        return found;
      }
    }
    return nullptr;
  };
  EXPECT_NE(find_filtered_scan(plan), nullptr) << "the a.y > 10 predicate did not reach a scan";
}

TEST(OptimizerTest, TwoTableJoinBuildsOnTheSmallerSide) {
  auto catalog = MakeTestCatalog();
  // The physical INNER hash join always builds child 0, and the planner emits FROM order — so
  // `FROM t2_10k, t1_1k` would build a 10k-row hash table to probe 1k rows. The join-order pass
  // must swap the smaller estimated side into the build slot (and restore the column order with a
  // projection on top).
  auto plan = TryOptimize(*catalog, "SELECT * FROM t2_10k, t1_1k WHERE v3 = v1");

  const auto join_node = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join_node, nullptr);
  const auto &join = dynamic_cast<const HashJoinPlanNode &>(*join_node);
  ASSERT_EQ(join.GetLeftPlan()->GetType(), PlanType::SeqScan);
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join.GetLeftPlan()).table_name_, "t1_1k")
      << "the smaller side must be child 0, the build side";

  // The FROM order that already builds on the smaller side is left exactly as it was.
  auto untouched = TryOptimize(*catalog, "SELECT * FROM t1_1k, t2_10k WHERE v1 = v3");
  const auto j2 = FindPlanNode(untouched, PlanType::HashJoin);
  ASSERT_NE(j2, nullptr);
  const auto &join2 = dynamic_cast<const HashJoinPlanNode &>(*j2);
  ASSERT_EQ(join2.GetLeftPlan()->GetType(), PlanType::SeqScan);
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join2.GetLeftPlan()).table_name_, "t1_1k");
}

TEST(OptimizerTest, ColumnPruningAnnotatesTheJoinBuildSide) {
  auto catalog = MakeTestCatalog();
  // Only b.y is read from the build side above the join (v3=b.x is a key, stored in its own layout
  // slot), so the annotation must carry exactly that column — the physical join then stores and
  // gathers one column instead of the build child's full width.
  auto plan = TryOptimize(*catalog, "SELECT a.x, b.y FROM a, b WHERE a.x = b.x");
  const auto join_node = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join_node, nullptr);
  const auto &join = dynamic_cast<const HashJoinPlanNode &>(*join_node);
  ASSERT_TRUE(join.build_live_annotated_);
  // The build side is whichever child the physical convention hashes (left for INNER); its live
  // set holds exactly the one payload column the SELECT reads from that side.
  EXPECT_EQ(join.build_live_columns_.size(), 1U);
}

TEST(OptimizerTest, FilterPushDownDoesNotCrossAnOuterJoin) {
  auto catalog = MakeTestCatalog();
  // The inner join to `d` must become a hash join, but the WHERE predicate on the
  // LEFT join's output must NOT be pushed into the LEFT join — it runs after
  // null-padding. It settles as a Filter directly above the preserved LEFT join.
  auto plan = TryOptimize(*catalog, "SELECT * FROM (a LEFT JOIN b ON a.x = b.x), d WHERE a.x = d.x AND a.y > 10");

  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 2U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 0U);
  // Exactly one Filter survives — the `a.y > 10` sitting above the LEFT join.
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 1U);

  auto left_join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(left_join, nullptr);
  // Neither scan under the LEFT join absorbed the WHERE predicate.
  const auto &filter = dynamic_cast<const FilterPlanNode &>(*FindPlanNode(plan, PlanType::Filter));
  EXPECT_EQ(filter.GetChildAt(0)->GetType(), PlanType::HashJoin);
  const auto &left = dynamic_cast<const HashJoinPlanNode &>(*filter.GetChildAt(0));
  EXPECT_EQ(left.GetJoinType(), JoinType::LEFT);
}

TEST(OptimizerTest, FilterPushDownLeavesAPureJoinPredicateAlone) {
  auto catalog = MakeTestCatalog();
  auto plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.x");
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  // Nothing to push: both scans stay bare.
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(0)).filter_predicate_, nullptr);
  EXPECT_EQ(dynamic_cast<const SeqScanPlanNode &>(*join->GetChildAt(1)).filter_predicate_, nullptr);
}

// ---------------------------------------------------------------------------
// ColumnPruning is a stub. This test documents that, and will fail loudly when
// someone implements it — which is the point.
// ---------------------------------------------------------------------------

TEST(OptimizerTest, ColumnPruningIsStillAStub) {
  auto catalog = MakeTestCatalog();
  // The scan reads all five columns of `y` even though only `x` is ever used.
  auto plan = TryOptimize(*catalog, "SELECT x FROM y");
  auto scan = FindPlanNode(plan, PlanType::SeqScan);
  ASSERT_NE(scan, nullptr);
  EXPECT_EQ(scan->OutputSchema().GetColumnCount(), 5U) << "column pruning appears to be implemented; update this test";
}

// ---------------------------------------------------------------------------
// Pass skipping: a rewrite whose trigger node is absent walks AND rebuilds the tree for nothing,
// which is real time on the small statements a DML workload is made of. Skipping it must not
// change any plan — these pin the shapes that DO need each group of passes.
// ---------------------------------------------------------------------------

TEST(OptimizerTest, SkippingInapplicablePassesLeavesPlansUnchanged) {
  auto catalog = MakeTestCatalog();
  // A join query still gets the whole join/filter group: cross product -> hash join, filter merged.
  auto join_plan = TryOptimize(*catalog, "SELECT * FROM a, b WHERE a.x = b.y");
  EXPECT_EQ(CountPlanNodes(join_plan, PlanType::HashJoin), 1U);
  EXPECT_EQ(CountPlanNodes(join_plan, PlanType::NestedLoopJoin), 0U);

  // A filter with no join still reaches the scan.
  auto filter_plan = TryOptimize(*catalog, "SELECT x FROM y WHERE z = 1");
  auto scan = FindPlanNode(filter_plan, PlanType::SeqScan);
  ASSERT_NE(scan, nullptr);
  EXPECT_NE(dynamic_cast<const SeqScanPlanNode &>(*scan).filter_predicate_, nullptr);

  // ORDER BY + LIMIT still collapses to TopN.
  auto topn_plan = TryOptimize(*catalog, "SELECT x FROM y ORDER BY x LIMIT 3");
  EXPECT_EQ(CountPlanNodes(topn_plan, PlanType::TopN), 1U);
  EXPECT_EQ(CountPlanNodes(topn_plan, PlanType::Sort), 0U);

  // And a plan with none of those trigger nodes (INSERT ... VALUES) survives with its shape intact.
  auto insert_plan = TryOptimize(*catalog, "INSERT INTO a VALUES (1, 2)");
  EXPECT_EQ(CountPlanNodes(insert_plan, PlanType::Insert), 1U);
  EXPECT_EQ(CountPlanNodes(insert_plan, PlanType::Values), 1U);
}

}  // namespace bumblebee
