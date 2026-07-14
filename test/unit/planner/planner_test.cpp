//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// planner_test.cpp
//
// Identification: test/unit/planner/planner_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "planner/planner.h"

#include "execution/plans/aggregation_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/insert_plan.h"
#include "execution/plans/limit_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/update_plan.h"
#include "execution/plans/values_plan.h"
#include "frontend_test_util.h"
#include "gtest/gtest.h"

namespace bumblebee {

namespace {

/** @brief Bind and plan a query, without optimizing it. */
auto TryPlan(const Catalog &catalog, const std::string &query) -> AbstractPlanNodeRef {
  auto statements = TryBind(catalog, query);
  EXPECT_EQ(statements.size(), 1U);
  Planner planner(catalog);
  planner.PlanQuery(*statements[0]);
  return planner.plan_;
}

}  // namespace

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

TEST(PlannerTest, PlanSelectStar) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT * FROM y");
  EXPECT_EQ(plan->GetType(), PlanType::Projection);
  EXPECT_EQ(plan->OutputSchema().GetColumnCount(), 5U);
  ASSERT_EQ(plan->GetChildren().size(), 1U);
  EXPECT_EQ(plan->GetChildAt(0)->GetType(), PlanType::SeqScan);
}

TEST(PlannerTest, PlanSelectConstant) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT 1");
  // No FROM: a single empty row for the projection to evaluate over.
  EXPECT_EQ(plan->GetType(), PlanType::Projection);
  EXPECT_EQ(plan->GetChildAt(0)->GetType(), PlanType::Values);
}

TEST(PlannerTest, PlanWhere) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT x FROM y WHERE z = 1");
  EXPECT_EQ(plan->GetType(), PlanType::Projection);
  const auto &filter = plan->GetChildAt(0);
  ASSERT_EQ(filter->GetType(), PlanType::Filter);
  EXPECT_EQ(dynamic_cast<const FilterPlanNode &>(*filter).GetPredicate()->ToString(), "(#0.1=1)");
  EXPECT_EQ(filter->GetChildAt(0)->GetType(), PlanType::SeqScan);
}

TEST(PlannerTest, PlanCrossProduct) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT * FROM a, b");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 1U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::SeqScan), 2U);

  auto join = FindPlanNode(plan, PlanType::NestedLoopJoin);
  ASSERT_NE(join, nullptr);
  // A cross product's predicate is the constant true; the optimizer relies on this.
  EXPECT_EQ(dynamic_cast<const NestedLoopJoinPlanNode &>(*join).Predicate()->ToString(), "true");
  EXPECT_EQ(join->OutputSchema().GetColumnCount(), 4U);
}

TEST(PlannerTest, PlanInnerJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT * FROM a INNER JOIN b ON a.x = b.y");
  auto join = FindPlanNode(plan, PlanType::NestedLoopJoin);
  ASSERT_NE(join, nullptr);
  // The two sides are addressed separately: #0 is the left input, #1 the right.
  EXPECT_EQ(dynamic_cast<const NestedLoopJoinPlanNode &>(*join).Predicate()->ToString(), "(#0.0=#1.1)");
}

TEST(PlannerTest, PlanThreeWayJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT * FROM a, b, y");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::NestedLoopJoin), 2U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::SeqScan), 3U);
}

TEST(PlannerTest, PlanAggregation) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT z, max(a) FROM y GROUP BY z HAVING max(a) > 0");
  // Projection -> Filter (the HAVING) -> Aggregation -> SeqScan
  EXPECT_EQ(plan->GetType(), PlanType::Projection);
  const auto &having = plan->GetChildAt(0);
  ASSERT_EQ(having->GetType(), PlanType::Filter);
  const auto &agg = having->GetChildAt(0);
  ASSERT_EQ(agg->GetType(), PlanType::Aggregation);

  const auto &agg_plan = dynamic_cast<const AggregationPlanNode &>(*agg);
  EXPECT_EQ(agg_plan.GetGroupBys().size(), 1U);
  // Two max() calls: the one in the select list and the one in the HAVING.
  ASSERT_EQ(agg_plan.GetAggregateTypes().size(), 2U);
  EXPECT_EQ(agg_plan.GetAggregateTypes()[0], AggregationType::MaxAggregate);
  EXPECT_EQ(agg->GetChildAt(0)->GetType(), PlanType::SeqScan);
}

TEST(PlannerTest, PlanCountStar) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT count(*) FROM y");
  auto agg = FindPlanNode(plan, PlanType::Aggregation);
  ASSERT_NE(agg, nullptr);
  const auto &agg_plan = dynamic_cast<const AggregationPlanNode &>(*agg);
  EXPECT_TRUE(agg_plan.GetGroupBys().empty());
  ASSERT_EQ(agg_plan.GetAggregateTypes().size(), 1U);
  EXPECT_EQ(agg_plan.GetAggregateTypes()[0], AggregationType::CountStarAggregate);
}

TEST(PlannerTest, PlanAggregateOverExpression) {
  auto catalog = MakeTestCatalog();
  // The two max() calls are computed *by* the aggregation; the `+` is computed
  // *over* its output. This is the whole reason aggregation is planned in two passes.
  auto plan = TryPlan(*catalog, "SELECT max(x) + max(z) FROM y");
  auto agg = FindPlanNode(plan, PlanType::Aggregation);
  ASSERT_NE(agg, nullptr);
  EXPECT_EQ(dynamic_cast<const AggregationPlanNode &>(*agg).GetAggregateTypes().size(), 2U);
  ASSERT_EQ(plan->GetType(), PlanType::Projection);
  EXPECT_EQ(dynamic_cast<const ProjectionPlanNode &>(*plan).GetExpressions()[0]->ToString(), "(#0.0+#0.1)");
}

TEST(PlannerTest, PlanOrderByLimit) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT * FROM y ORDER BY x LIMIT 10");
  ASSERT_EQ(plan->GetType(), PlanType::Limit);
  EXPECT_EQ(dynamic_cast<const LimitPlanNode &>(*plan).GetLimit(), 10U);
  const auto &sort = plan->GetChildAt(0);
  ASSERT_EQ(sort->GetType(), PlanType::Sort);
  EXPECT_EQ(dynamic_cast<const SortPlanNode &>(*sort).GetOrderBy().size(), 1U);
}

TEST(PlannerTest, PlanSubqueryInFrom) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT * FROM (SELECT x, z FROM y) s WHERE s.x > 1");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::SeqScan), 1U);
  // The subquery's own projection, plus the renaming projection, plus the outer one.
  EXPECT_GE(CountPlanNodes(plan, PlanType::Projection), 2U);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::Filter), 1U);
}

TEST(PlannerTest, PlanDistinct) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT DISTINCT x FROM y");
  // DISTINCT is a group-by over every output column with no aggregates.
  auto agg = FindPlanNode(plan, PlanType::Aggregation);
  ASSERT_NE(agg, nullptr);
  const auto &agg_plan = dynamic_cast<const AggregationPlanNode &>(*agg);
  EXPECT_EQ(agg_plan.GetGroupBys().size(), 1U);
  EXPECT_TRUE(agg_plan.GetAggregates().empty());
}

TEST(PlannerTest, PlanStringFunction) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT lower(x) FROM c");
  ASSERT_EQ(plan->GetType(), PlanType::Projection);
  EXPECT_EQ(dynamic_cast<const ProjectionPlanNode &>(*plan).GetExpressions()[0]->ToString(), "lower(#0.0)");
}

// ---------------------------------------------------------------------------
// DML
// ---------------------------------------------------------------------------

TEST(PlannerTest, PlanInsertValues) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "INSERT INTO a VALUES (1, 2), (3, 4)");
  ASSERT_EQ(plan->GetType(), PlanType::Insert);
  EXPECT_EQ(dynamic_cast<const InsertPlanNode &>(*plan).GetTableOid(), catalog->GetTable("a")->oid_);

  auto values = FindPlanNode(plan, PlanType::Values);
  ASSERT_NE(values, nullptr);
  EXPECT_EQ(dynamic_cast<const ValuesPlanNode &>(*values).GetValues().size(), 2U);
}

TEST(PlannerTest, PlanInsertSelect) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "INSERT INTO a SELECT x, y FROM b");
  EXPECT_EQ(plan->GetType(), PlanType::Insert);
  EXPECT_EQ(CountPlanNodes(plan, PlanType::SeqScan), 1U);
}

TEST(PlannerTest, PlanInsertSchemaMismatchThrows) {
  auto catalog = MakeTestCatalog();
  // `a` is (INT, INT); these are strings.
  EXPECT_THROW(TryPlan(*catalog, "INSERT INTO a VALUES ('x', 'y')"), Exception);
  // Too few columns.
  EXPECT_THROW(TryPlan(*catalog, "INSERT INTO a VALUES (1)"), Exception);
}

TEST(PlannerTest, PlanDelete) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "DELETE FROM y WHERE z = 1");
  ASSERT_EQ(plan->GetType(), PlanType::Delete);
  const auto &filter = plan->GetChildAt(0);
  ASSERT_EQ(filter->GetType(), PlanType::Filter);
  EXPECT_EQ(filter->GetChildAt(0)->GetType(), PlanType::SeqScan);
}

TEST(PlannerTest, PlanUpdate) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "UPDATE y SET z = z + 1 WHERE x = 1");
  ASSERT_EQ(plan->GetType(), PlanType::Update);
  const auto &update = dynamic_cast<const UpdatePlanNode &>(*plan);
  // One target expression per table column, not just the one in the SET clause: the
  // untouched columns get a reference to their own old value.
  ASSERT_EQ(update.target_expressions_.size(), 5U);
  EXPECT_EQ(update.target_expressions_[1]->ToString(), "(#0.1+1)");
  EXPECT_EQ(update.target_expressions_[0]->ToString(), "#0.0");
}

// ---------------------------------------------------------------------------
// A golden string, as a regression net over the whole ToString layer
// ---------------------------------------------------------------------------

TEST(PlannerTest, PlanToStringGolden) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT x FROM y WHERE z = 1");
  EXPECT_EQ(plan->ToString(false),
            "Projection { exprs=[\"#0.0\"] }\n"
            "  Filter { predicate=(#0.1=1) }\n"
            "    SeqScan { table=y }");
}

}  // namespace bumblebee
