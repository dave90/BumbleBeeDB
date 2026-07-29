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

#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/subquery_expression.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
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

// ---------------------------------------------------------------------------
// Scalar subqueries
// ---------------------------------------------------------------------------

TEST(PlannerTest, ScalarSubqueryPreExecutesToConstant) {
  auto catalog = MakeTestCatalog();
  auto statements = TryBind(*catalog, "SELECT x FROM a WHERE x = (SELECT max(x) FROM b)");
  ASSERT_EQ(statements.size(), 1U);

  Planner planner(*catalog);
  size_t evals = 0;
  planner.subquery_eval_ = [&evals](const AbstractPlanNodeRef &subplan) {
    evals++;
    EXPECT_EQ(subplan->OutputSchema().GetColumnCount(), 1U);
    return Value(42);
  };
  planner.PlanQuery(*statements[0]);

  // The hook ran exactly once and its value was substituted as a literal in the filter.
  EXPECT_EQ(evals, 1U);
  auto filter = FindPlanNode(planner.plan_, PlanType::Filter);
  ASSERT_NE(filter, nullptr);
  const auto &pred = dynamic_cast<const FilterPlanNode &>(*filter).GetPredicate();
  ASSERT_EQ(pred->GetChildren().size(), 2U);
  const auto *cst = dynamic_cast<const ConstantValueExpression *>(pred->GetChildAt(1).get());
  ASSERT_NE(cst, nullptr);
  EXPECT_EQ(cst->val_.GetAs<int>(), 42);
}

TEST(PlannerTest, ScalarSubqueryWithoutEvalHookKeepsPlaceholder) {
  // The EXPLAIN path: no eval hook, so nothing may run — the subquery stays a placeholder.
  auto catalog = MakeTestCatalog();
  auto statements = TryBind(*catalog, "SELECT x FROM a WHERE x = (SELECT max(x) FROM b)");
  Planner planner(*catalog);
  planner.PlanQuery(*statements[0]);

  auto filter = FindPlanNode(planner.plan_, PlanType::Filter);
  ASSERT_NE(filter, nullptr);
  const auto &pred = dynamic_cast<const FilterPlanNode &>(*filter).GetPredicate();
  const auto *placeholder = dynamic_cast<const SubqueryExpression *>(pred->GetChildAt(1).get());
  ASSERT_NE(placeholder, nullptr);
  ASSERT_NE(placeholder->subplan_, nullptr);
  EXPECT_EQ(placeholder->subplan_->OutputSchema().GetColumnCount(), 1U);
}

TEST(PlannerTest, ScalarSubqueryMustReturnOneColumn) {
  auto catalog = MakeTestCatalog();
  EXPECT_THROW(TryBind(*catalog, "SELECT x FROM a WHERE x = (SELECT x, y FROM b)"), BinderException);
}

TEST(PlannerTest, InSubqueryPlansAsNullAwareSemiJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT x FROM a WHERE x IN (SELECT y FROM b)");

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.GetJoinType(), JoinType::SEMI);
  EXPECT_TRUE(hash_join.null_aware_);
  // Output = the outer schema untouched; the build (right) side is the subquery's single column.
  EXPECT_EQ(join->OutputSchema().GetColumnCount(), hash_join.GetLeftPlan()->OutputSchema().GetColumnCount());
  EXPECT_EQ(hash_join.GetRightPlan()->OutputSchema().GetColumnCount(), 1U);
  ASSERT_EQ(hash_join.LeftJoinKeyExpressions().size(), 1U);
  ASSERT_EQ(hash_join.RightJoinKeyExpressions().size(), 1U);
}

TEST(PlannerTest, NotInSubqueryPlansAsAntiJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT x FROM a WHERE x NOT IN (SELECT y FROM b) AND y > 0");

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.GetJoinType(), JoinType::ANTI);
  EXPECT_TRUE(hash_join.null_aware_);
  // The ordinary conjunct stays a Filter BELOW the anti join (over the FROM plan).
  auto filter = FindPlanNode(hash_join.GetLeftPlan(), PlanType::Filter);
  EXPECT_NE(filter, nullptr);
}

TEST(PlannerTest, CorrelatedExistsDecorrelatesToSemiJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "SELECT a.x FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.y = a.x)");

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.GetJoinType(), JoinType::SEMI);
  // EXISTS semantics, not IN's: a NULL key row simply never matches.
  EXPECT_FALSE(hash_join.null_aware_);
  // The correlation equality became the join keys; the subquery's output is exactly those keys.
  ASSERT_EQ(hash_join.LeftJoinKeyExpressions().size(), 1U);
  ASSERT_EQ(hash_join.RightJoinKeyExpressions().size(), 1U);
  EXPECT_EQ(hash_join.GetRightPlan()->OutputSchema().GetColumnCount(), 1U);
}

TEST(PlannerTest, CorrelatedScalarAggregateDecorrelatesToLeftJoin) {
  auto catalog = MakeTestCatalog();
  auto plan =
      TryPlan(*catalog, "SELECT a.x FROM a WHERE a.x > (SELECT max(v1) FROM t1_1k WHERE v2 = a.y)");

  // The subquery became `... GROUP BY v2` LEFT-joined on the correlation key, with the comparison
  // filtering the join output (whose schema is the outer columns plus key + aggregate).
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.GetJoinType(), JoinType::LEFT);
  auto agg = FindPlanNode(hash_join.GetRightPlan(), PlanType::Aggregation);
  ASSERT_NE(agg, nullptr);
  EXPECT_EQ(dynamic_cast<const AggregationPlanNode &>(*agg).GetGroupBys().size(), 1U);
  const auto outer_width = hash_join.GetLeftPlan()->OutputSchema().GetColumnCount();
  EXPECT_EQ(join->OutputSchema().GetColumnCount(), outer_width + 2);  // + key + aggregate

  auto filter = FindPlanNode(plan, PlanType::Filter);
  ASSERT_NE(filter, nullptr);
  EXPECT_EQ(filter->GetChildAt(0)->GetType(), PlanType::HashJoin);
}

TEST(PlannerTest, CorrelatedScalarRestrictsSubqueryToTheProbedKeys) {
  auto catalog = MakeTestCatalog();
  // `a.y = 5` narrows the outer query to a handful of correlation keys, so the decorrelated
  // aggregate must not group the WHOLE of t1_1k: a SEMI join against those keys goes under it.
  auto plan = TryPlan(*catalog,
                      "SELECT a.x FROM a WHERE a.y = 5 AND a.x > (SELECT max(v1) FROM t1_1k WHERE v2 = a.y)");

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  ASSERT_EQ(dynamic_cast<const HashJoinPlanNode &>(*join).GetJoinType(), JoinType::LEFT);
  auto agg = FindPlanNode(join->GetChildAt(1), PlanType::Aggregation);
  ASSERT_NE(agg, nullptr);
  ASSERT_EQ(agg->GetChildren().size(), 1U);

  const auto &restriction = agg->GetChildAt(0);
  ASSERT_EQ(restriction->GetType(), PlanType::HashJoin);
  const auto &semi = dynamic_cast<const HashJoinPlanNode &>(*restriction);
  EXPECT_EQ(semi.GetJoinType(), JoinType::SEMI);
  // A SEMI join emits its left side only, so the aggregation's expressions still index the same
  // columns they did before the splice.
  EXPECT_EQ(semi.OutputSchema().GetColumnCount(), semi.GetLeftPlan()->OutputSchema().GetColumnCount());
  EXPECT_EQ(semi.GetLeftPlan()->GetType(), PlanType::SeqScan);
  // The key source: the outer query's own table, carrying the conjuncts that mention only it.
  auto key_filter = semi.GetRightPlan();
  ASSERT_EQ(key_filter->GetType(), PlanType::Filter);
  EXPECT_EQ(dynamic_cast<const FilterPlanNode &>(*key_filter).GetPredicate()->ToString(), "(#0.1=5)");
  EXPECT_EQ(key_filter->GetChildAt(0)->GetType(), PlanType::SeqScan);
}

TEST(PlannerTest, CorrelatedScalarWithoutOuterFilterIsNotRestricted) {
  auto catalog = MakeTestCatalog();
  // Nothing narrows the outer query, so the key source would be every value of the column: the
  // extra scan would buy nothing and the rewrite stays out of the plan.
  auto plan = TryPlan(*catalog, "SELECT a.x FROM a WHERE a.x > (SELECT max(v1) FROM t1_1k WHERE v2 = a.y)");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 1U);  // the decorrelating LEFT join, no more
}

TEST(PlannerTest, CorrelatedScalarRestrictionSkipsCrossTableConjuncts) {
  auto catalog = MakeTestCatalog();
  // `a.y = b.y` spans two tables, so it cannot be re-evaluated over the key table alone; `b.x = 7`
  // does not mention the key table at all. Neither can restrict the keys, and no rewrite happens.
  auto plan = TryPlan(*catalog,
                      "SELECT a.x FROM a, b WHERE a.y = b.y AND b.x = 7 AND "
                      "a.x > (SELECT max(v1) FROM t1_1k WHERE v2 = a.y)");
  EXPECT_EQ(CountPlanNodes(plan, PlanType::HashJoin), 1U);
}

TEST(PlannerTest, CorrelatedScalarOutsideWhereIsRejected) {
  auto catalog = MakeTestCatalog();
  auto statements = TryBind(*catalog, "SELECT (SELECT max(v1) FROM t1_1k WHERE v2 = a.y) FROM a");
  Planner planner(*catalog);
  EXPECT_THROW(planner.PlanQuery(*statements[0]), NotImplementedException);
}

// UPDATE / DELETE share PlanSelect's WHERE handling, so a subquery conjunct becomes a SEMI/ANTI
// join over the scan rather than a filter predicate the expression planner cannot express.
TEST(PlannerTest, UpdateFlattensInSubqueryToSemiJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "UPDATE a SET y = y + 1 WHERE x IN (SELECT y FROM b)");

  EXPECT_EQ(plan->GetType(), PlanType::Update);
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  const auto &hash_join = dynamic_cast<const HashJoinPlanNode &>(*join);
  EXPECT_EQ(hash_join.GetJoinType(), JoinType::SEMI);
  EXPECT_TRUE(hash_join.null_aware_);  // IN's three-valued semantics
  // The join emits its left side only, so the update still sees the table's own schema.
  EXPECT_EQ(join->OutputSchema().GetColumnCount(), hash_join.GetLeftPlan()->OutputSchema().GetColumnCount());
  EXPECT_EQ(hash_join.GetLeftPlan()->GetType(), PlanType::SeqScan);
}

TEST(PlannerTest, DeleteFlattensNotInSubqueryToAntiJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "DELETE FROM a WHERE x NOT IN (SELECT y FROM b)");

  EXPECT_EQ(plan->GetType(), PlanType::Delete);
  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  EXPECT_EQ(dynamic_cast<const HashJoinPlanNode &>(*join).GetJoinType(), JoinType::ANTI);
}

TEST(PlannerTest, DeleteKeepsOrdinaryConjunctsBelowTheSubqueryJoin) {
  auto catalog = MakeTestCatalog();
  auto plan = TryPlan(*catalog, "DELETE FROM a WHERE y = 3 AND x IN (SELECT y FROM b)");

  auto join = FindPlanNode(plan, PlanType::HashJoin);
  ASSERT_NE(join, nullptr);
  // The ordinary conjunct is a Filter directly over the scan, UNDER the join — the shape the
  // pushdown/merge passes and the DML lowering both expect.
  const auto &left = dynamic_cast<const HashJoinPlanNode &>(*join).GetLeftPlan();
  ASSERT_EQ(left->GetType(), PlanType::Filter);
  EXPECT_EQ(left->GetChildAt(0)->GetType(), PlanType::SeqScan);
}

// A correlated scalar has no enclosing SELECT to decorrelate against in an UPDATE/DELETE, so it
// must still fail cleanly rather than plan something wrong.
TEST(PlannerTest, UpdateWithCorrelatedScalarIsRejected) {
  auto catalog = MakeTestCatalog();
  auto statements = TryBind(*catalog, "UPDATE a SET y = 1 WHERE x > (SELECT max(v1) FROM t1_1k WHERE v2 = a.y)");
  Planner planner(*catalog);
  EXPECT_THROW(planner.PlanQuery(*statements[0]), NotImplementedException);
}

TEST(PlannerTest, InSubqueryOnlyAsTopLevelConjunct) {
  auto catalog = MakeTestCatalog();
  auto statements = TryBind(*catalog, "SELECT x FROM a WHERE x IN (SELECT y FROM b) OR x = 1");
  Planner planner(*catalog);
  EXPECT_THROW(planner.PlanQuery(*statements[0]), NotImplementedException);
}

}  // namespace bumblebee
