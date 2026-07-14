//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// binder_test.cpp
//
// Identification: test/unit/binder/binder_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_statement.h"
#include "binder/statement/create_statement.h"
#include "binder/statement/explain_statement.h"
#include "binder/statement/select_statement.h"
#include "common/enums/statement_type.h"
#include "common/exception.h"
#include "common/util/string_util.h"
#include "frontend_test_util.h"
#include "gtest/gtest.h"

namespace bumblebee {

namespace {

/** @brief Bind `query` against the standard test catalog. */
auto Bind(const std::string &query) -> std::vector<std::unique_ptr<BoundStatement>> {
  auto catalog = MakeTestCatalog();
  return TryBind(*catalog, query);
}

/** @brief Bind `query` and assert it produced exactly one statement of type `type`. Returns its ToString(). */
auto BindOne(const std::string &query, StatementType type) -> std::string {
  auto statements = Bind(query);
  EXPECT_EQ(statements.size(), 1U) << query;
  EXPECT_EQ(statements[0]->type_, type) << query;
  return statements[0]->ToString();
}

/** @brief gtest predicate: does `haystack` contain `needle`? */
auto Contains(const std::string &haystack, const std::string &needle) -> testing::AssertionResult {
  if (StringUtil::Contains(haystack, needle)) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << "expected to find `" << needle << "` in:\n" << haystack;
}

}  // namespace

// ===--------------------------------------------------------------------=== //
// SELECT
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindSelectValue) {
  auto out = BindOne("select 1", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundSelect"));
  EXPECT_TRUE(Contains(out, "columns=[1]"));
  // No FROM clause, so the table ref is the empty placeholder.
  EXPECT_TRUE(Contains(out, "<empty>"));
}

TEST(BinderTest, BindSelectFrom) {
  auto out = BindOne("select x from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundBaseTableRef { table=y"));
  EXPECT_TRUE(Contains(out, "columns=[y.x]"));
}

TEST(BinderTest, BindSelectFromWhere) {
  auto out = BindOne("select x from y where z = 1", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "columns=[y.x]"));
  EXPECT_TRUE(Contains(out, "where=(y.z=1)"));
}

TEST(BinderTest, BindSelectStar) {
  auto out = BindOne("select * from y", StatementType::SELECT_STATEMENT);
  // `*` is expanded by the binder; it must not survive into the bound tree.
  EXPECT_FALSE(StringUtil::Contains(out, "columns=[*]"));
  EXPECT_TRUE(Contains(out, "columns=[y.x, y.z, y.a, y.b, y.c]"));
}

TEST(BinderTest, BindSelectMultipleColumns) {
  auto out = BindOne("select x, z, b from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "columns=[y.x, y.z, y.b]"));
}

TEST(BinderTest, BindSelectQualifiedColumn) {
  auto out = BindOne("select y.x from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "columns=[y.x]"));
}

TEST(BinderTest, BindSelectExpression) {
  auto out = BindOne("select x + z * 2 from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "columns=[(y.x+(y.z*2))]"));
}

TEST(BinderTest, BindUnaryOp) {
  auto out = BindOne("select -x from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "columns=[(-y.x)]"));
}

TEST(BinderTest, BindNotOp) {
  auto out = BindOne("select x from y where not (z = 1)", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "where=(not(y.z=1))"));
}

TEST(BinderTest, BindBooleanBinaryOp) {
  auto out = BindOne("select x from y where z = 1 and b = 2", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "where=((y.z=1)and(y.b=2))"));
}

TEST(BinderTest, BindAlias) {
  auto out = BindOne("select x as col1, z as col2 from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "(y.x as col1)"));
  EXPECT_TRUE(Contains(out, "(y.z as col2)"));
}

TEST(BinderTest, BindTableAlias) {
  auto out = BindOne("select t.x from y as t", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "alias=t"));
  EXPECT_TRUE(Contains(out, "columns=[t.x]"));
}

TEST(BinderTest, BindFuncCall) {
  auto out = BindOne("select lower(x) from c", StatementType::SELECT_STATEMENT);
  // fmt renders the argument vector as a list, so a one-argument call prints as `f([arg])`.
  EXPECT_TRUE(Contains(out, "columns=[lower([c.x])]"));
}

TEST(BinderTest, BindDistinct) {
  auto out = BindOne("select distinct x from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "is_distinct=true"));
}

// ===--------------------------------------------------------------------=== //
// Aggregation
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindAggregation) {
  auto out = BindOne("select sum(x), min(z), max(b) from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "sum([y.x])"));
  EXPECT_TRUE(Contains(out, "min([y.z])"));
  EXPECT_TRUE(Contains(out, "max([y.b])"));
}

TEST(BinderTest, BindCountStar) {
  // `count(*)` counts rows, not values: the binder rewrites it to `count_star()`.
  auto out = BindOne("select count(*) from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "count_star([])"));
}

TEST(BinderTest, BindDistinctAggregation) {
  auto out = BindOne("select count(distinct x) from y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "count_distinct([y.x])"));
}

TEST(BinderTest, BindGroupByHaving) {
  auto out = BindOne("select x, count(z) from y group by x having count(z) > 1", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "groupBy=[y.x]"));
  EXPECT_TRUE(Contains(out, "having=(count([y.z])>1)"));
}

// ===--------------------------------------------------------------------=== //
// Joins
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindCrossJoin) {
  auto out = BindOne("select * from a, b", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundCrossProductRef"));
  EXPECT_TRUE(Contains(out, "columns=[a.x, a.y, b.x, b.y]"));
}

TEST(BinderTest, BindInnerJoin) {
  auto out = BindOne("select * from a inner join b on a.x = b.x", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundJoin { type=Inner"));
  EXPECT_TRUE(Contains(out, "condition=(a.x=b.x)"));
  EXPECT_TRUE(Contains(out, "columns=[a.x, a.y, b.x, b.y]"));
}

TEST(BinderTest, BindLeftJoin) {
  auto out = BindOne("select * from a left join b on a.x = b.x", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundJoin { type=Left"));
}

TEST(BinderTest, BindThreeWayJoin) {
  auto out = BindOne("select * from a inner join b on a.x = b.x inner join y on a.x = y.x",
                     StatementType::SELECT_STATEMENT);
  // A left-deep tree: (a join b) join y.
  EXPECT_TRUE(Contains(out, "BoundJoin"));
  EXPECT_TRUE(Contains(out, "condition=(a.x=b.x)"));
  EXPECT_TRUE(Contains(out, "condition=(a.x=y.x)"));
  EXPECT_TRUE(Contains(out, "columns=[a.x, a.y, b.x, b.y, y.x, y.z, y.a, y.b, y.c]"));
}

TEST(BinderTest, BindThreeWayCrossProduct) {
  auto out = BindOne("select * from a, b, y", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundCrossProductRef"));
  EXPECT_TRUE(Contains(out, "columns=[a.x, a.y, b.x, b.y, y.x, y.z, y.a, y.b, y.c]"));
}

// ===--------------------------------------------------------------------=== //
// Sorting and limits
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindOrderBy) {
  auto out = BindOne("select x from y order by z desc, b asc", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "type=Descending, nulls=Default, expr=y.z"));
  EXPECT_TRUE(Contains(out, "type=Ascending, nulls=Default, expr=y.b"));
}

TEST(BinderTest, BindLimitOffset) {
  auto out = BindOne("select x from y limit 10 offset 5", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "limit=10"));
  EXPECT_TRUE(Contains(out, "offset=5"));
}

// ===--------------------------------------------------------------------=== //
// Subqueries and CTEs
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindFromSubquery) {
  auto out = BindOne("select s.x from (select x from y) s", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundSubqueryRef"));
  EXPECT_TRUE(Contains(out, "alias=s"));
  EXPECT_TRUE(Contains(out, "columns=[s.y.x]"));
}

TEST(BinderTest, BindUnnamedFromSubquery) {
  auto out = BindOne("select * from (select x from y)", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundSubqueryRef"));
  // Unnamed subqueries get a generated alias so that their columns can be referenced.
  EXPECT_TRUE(Contains(out, "__subquery#"));
}

TEST(BinderTest, BindCte) {
  auto out = BindOne("with cte1 as (select x from y) select * from cte1", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundCTERef { alias=cte1, cte=cte1 }"));
  // A CTE's columns keep the name they had inside it, qualified by the CTE's alias.
  EXPECT_TRUE(Contains(out, "columns=[cte1.y.x]"));
  EXPECT_TRUE(Contains(out, "ctes=BoundSubqueryRef"));
}

// ===--------------------------------------------------------------------=== //
// INSERT / UPDATE / DELETE
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindInsert) {
  auto out = BindOne("insert into y values (1, 2, 3, 4, 5)", StatementType::INSERT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundInsert"));
  EXPECT_TRUE(Contains(out, "table=BoundBaseTableRef { table=y"));
  EXPECT_TRUE(Contains(out, "BoundExpressionListRef"));
  EXPECT_TRUE(Contains(out, "[1, 2, 3, 4, 5]"));
}

TEST(BinderTest, BindInsertMultipleRows) {
  auto out = BindOne("insert into a values (1, 2), (3, 4)", StatementType::INSERT_STATEMENT);
  EXPECT_TRUE(Contains(out, "[1, 2]"));
  EXPECT_TRUE(Contains(out, "[3, 4]"));
}

TEST(BinderTest, BindInsertSelect) {
  auto out = BindOne("insert into a select x, y from b", StatementType::INSERT_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundInsert"));
  EXPECT_TRUE(Contains(out, "table=BoundBaseTableRef { table=a"));
  EXPECT_TRUE(Contains(out, "BoundBaseTableRef { table=b"));
  EXPECT_TRUE(Contains(out, "columns=[b.x, b.y]"));
}

TEST(BinderTest, BindUpdate) {
  auto out = BindOne("update y set x = 1 where z = 2", StatementType::UPDATE_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundUpdate"));
  EXPECT_TRUE(Contains(out, "filter_expr=(y.z=2)"));
  EXPECT_TRUE(Contains(out, "target_expr=[(y.x, 1)]"));
}

TEST(BinderTest, BindUpdateWithoutWhere) {
  // A WHERE-less UPDATE gets a constant `true` filter rather than a null one.
  auto out = BindOne("update y set x = 1", StatementType::UPDATE_STATEMENT);
  EXPECT_TRUE(Contains(out, "filter_expr=true"));
}

TEST(BinderTest, BindDelete) {
  auto out = BindOne("delete from y where x = 1", StatementType::DELETE_STATEMENT);
  EXPECT_TRUE(Contains(out, "BoundDelete"));
  EXPECT_TRUE(Contains(out, "table=BoundBaseTableRef { table=y"));
  EXPECT_TRUE(Contains(out, "expr=(y.x=1)"));
}

TEST(BinderTest, BindDeleteWithoutWhere) {
  auto out = BindOne("delete from y", StatementType::DELETE_STATEMENT);
  EXPECT_TRUE(Contains(out, "expr=true"));
}

// ===--------------------------------------------------------------------=== //
// CREATE TABLE
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindCreateTable) {
  auto statements = Bind("create table t(v1 int, v2 varchar(100), v3 bool, v4 double)");
  ASSERT_EQ(statements.size(), 1U);
  ASSERT_EQ(statements[0]->type_, StatementType::CREATE_STATEMENT);

  const auto &create = dynamic_cast<const CreateStatement &>(*statements[0]);
  EXPECT_EQ(create.table_, "t");
  ASSERT_EQ(create.columns_.size(), 4U);
  EXPECT_EQ(create.columns_[0].GetName(), "v1");
  EXPECT_EQ(create.columns_[0].GetType().ToString(), "INTEGER");
  EXPECT_EQ(create.columns_[1].GetType().ToString(), "VARCHAR");
  EXPECT_EQ(create.columns_[1].GetStorageSize(), 100U);
  EXPECT_EQ(create.columns_[2].GetType().ToString(), "BOOLEAN");
  EXPECT_EQ(create.columns_[3].GetType().ToString(), "DOUBLE");
  EXPECT_TRUE(create.primary_key_.empty());
}

TEST(BinderTest, BindCreateTablePrimaryKey) {
  auto statements = Bind("create table t(v1 int primary key, v2 int)");
  const auto &create = dynamic_cast<const CreateStatement &>(*statements[0]);
  ASSERT_EQ(create.primary_key_.size(), 1U);
  EXPECT_EQ(create.primary_key_[0], "v1");
}

TEST(BinderTest, BindCreateTableArrayColumns) {
  auto statements = Bind("create table t(v int, tags int[], fixed int[3])");
  ASSERT_EQ(statements.size(), 1U);
  ASSERT_EQ(statements[0]->type_, StatementType::CREATE_STATEMENT);

  const auto &create = dynamic_cast<const CreateStatement &>(*statements[0]);
  ASSERT_EQ(create.columns_.size(), 3U);

  EXPECT_EQ(create.columns_[0].GetName(), "v");
  EXPECT_EQ(create.columns_[0].GetType().ToString(), "INTEGER");

  // `int[]` is a variable-length LIST.
  EXPECT_EQ(create.columns_[1].GetName(), "tags");
  EXPECT_EQ(create.columns_[1].GetType().ToString(), "INTEGER[]");
  EXPECT_EQ(create.columns_[1].GetType().GetTypeId(), LogicalTypeId::LIST);
  EXPECT_FALSE(create.columns_[1].IsInlined());

  // `int[3]` is a fixed-length ARRAY.
  EXPECT_EQ(create.columns_[2].GetName(), "fixed");
  EXPECT_EQ(create.columns_[2].GetType().ToString(), "INTEGER[3]");
  EXPECT_EQ(create.columns_[2].GetType().GetTypeId(), LogicalTypeId::ARRAY);
  EXPECT_EQ(create.columns_[2].GetType().GetListData().size_, 3U);
  EXPECT_FALSE(create.columns_[2].IsInlined());
}

TEST(BinderTest, BindCreateTableArrayOfVarchar) {
  auto statements = Bind("create table t(names varchar[])");
  const auto &create = dynamic_cast<const CreateStatement &>(*statements[0]);
  ASSERT_EQ(create.columns_.size(), 1U);
  EXPECT_EQ(create.columns_[0].GetType().ToString(), "VARCHAR[]");
}

TEST(BinderTest, SelectFromTableWithArrayColumns) {
  auto out = BindOne("select id, tags, fixed from arr", StatementType::SELECT_STATEMENT);
  EXPECT_TRUE(Contains(out, "columns=[arr.id, arr.tags, arr.fixed]"));
}

// ===--------------------------------------------------------------------=== //
// EXPLAIN
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindExplain) {
  auto statements = Bind("explain select x from y");
  ASSERT_EQ(statements.size(), 1U);
  ASSERT_EQ(statements[0]->type_, StatementType::EXPLAIN_STATEMENT);

  const auto &explain = dynamic_cast<const ExplainStatement &>(*statements[0]);
  EXPECT_EQ(explain.statement_->type_, StatementType::SELECT_STATEMENT);
  // With no options given, EXPLAIN prints every stage.
  EXPECT_EQ(explain.options_, ExplainOptions::BINDER | ExplainOptions::PLANNER | ExplainOptions::OPTIMIZER |
                                  ExplainOptions::SCHEMA);
  EXPECT_TRUE(Contains(explain.ToString(), "BoundExplain"));
  EXPECT_TRUE(Contains(explain.ToString(), "BoundSelect"));
}

TEST(BinderTest, BindExplainWithOptions) {
  auto statements = Bind("explain (binder) select x from y");
  const auto &explain = dynamic_cast<const ExplainStatement &>(*statements[0]);
  EXPECT_EQ(explain.options_, ExplainOptions::BINDER);
}

// ===--------------------------------------------------------------------=== //
// Multiple statements
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, BindMultipleStatements) {
  auto statements = Bind("select x from y; delete from a; insert into b values (1, 2)");
  ASSERT_EQ(statements.size(), 3U);
  EXPECT_EQ(statements[0]->type_, StatementType::SELECT_STATEMENT);
  EXPECT_EQ(statements[1]->type_, StatementType::DELETE_STATEMENT);
  EXPECT_EQ(statements[2]->type_, StatementType::INSERT_STATEMENT);
}

// ===--------------------------------------------------------------------=== //
// Errors
// ===--------------------------------------------------------------------=== //

TEST(BinderTest, UnknownTable) { EXPECT_THROW(Bind("select x from nonexistent"), BinderException); }

TEST(BinderTest, UnknownColumn) { EXPECT_THROW(Bind("select nonexistent from y"), BinderException); }

TEST(BinderTest, UnknownColumnInWhere) { EXPECT_THROW(Bind("select x from y where nope = 1"), BinderException); }

TEST(BinderTest, AmbiguousColumnInCrossProduct) {
  // Both `a` and `b` have an `x`, so a bare `x` is ambiguous.
  EXPECT_THROW(Bind("select x from a, b"), BinderException);
}

TEST(BinderTest, AmbiguousColumnInJoin) {
  EXPECT_THROW(Bind("select x from a inner join b on a.x = b.x"), BinderException);
}

TEST(BinderTest, InsertIntoUnknownTable) {
  EXPECT_THROW(Bind("insert into nonexistent values (1)"), BinderException);
}

TEST(BinderTest, DeleteFromUnknownTable) { EXPECT_THROW(Bind("delete from nonexistent"), BinderException); }

TEST(BinderTest, SelectStarWithOtherExpressions) {
  EXPECT_THROW(Bind("select *, x from y"), BinderException);
}

TEST(BinderTest, SyntaxError) { EXPECT_THROW(Bind("select from from"), ParserException); }

TEST(BinderTest, UnsupportedColumnType) {
  EXPECT_THROW(Bind("create table t(v some_unknown_type)"), NotImplementedException);
}

TEST(BinderTest, WindowFunctionNotSupported) {
  // Window functions are deliberately out of scope for this binder.
  EXPECT_THROW(Bind("select sum(x) over (partition by z) from y"), NotImplementedException);
}

}  // namespace bumblebee
