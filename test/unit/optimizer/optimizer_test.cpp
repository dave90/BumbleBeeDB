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

#include "optimizer/optimizer.h"

#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/topn_plan.h"
#include "frontend_test_util.h"
#include "gtest/gtest.h"
#include "planner/planner.h"

namespace bumblebee {

namespace {

/** @brief Bind, plan and optimize a query. */
auto TryOptimize(const Catalog &catalog, const std::string &query) -> AbstractPlanNodeRef {
  auto statements = TryBind(catalog, query);
  EXPECT_EQ(statements.size(), 1U);
  Planner planner(catalog);
  planner.PlanQuery(*statements[0]);
  Optimizer optimizer(catalog);
  return optimizer.Optimize(planner.plan_);
}

/** @brief Bind and plan a query, without optimizing it. */
auto TryPlan(const Catalog &catalog, const std::string &query) -> AbstractPlanNodeRef {
  auto statements = TryBind(catalog, query);
  Planner planner(catalog);
  planner.PlanQuery(*statements[0]);
  return planner.plan_;
}

}  // namespace

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
  EXPECT_EQ(scan->OutputSchema().GetColumnCount(), 5U)
      << "column pruning appears to be implemented; update this test";
}

}  // namespace bumblebee
