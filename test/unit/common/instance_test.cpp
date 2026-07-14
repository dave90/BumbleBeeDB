//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// instance_test.cpp
//
// Identification: test/unit/common/instance_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "bumblebee_instance.h"

#include <sstream>

#include "common/exception.h"
#include "gtest/gtest.h"

namespace bumblebee {

namespace {

/** @brief Run a statement and return everything the instance wrote. */
auto RunSql(BumbleBeeInstance &instance, const std::string &sql) -> std::string {
  std::stringstream ss;
  SimpleStreamWriter writer(ss);
  instance.ExecuteSql(sql, writer);
  return ss.str();
}

}  // namespace

TEST(InstanceTest, CreateTable) {
  BumbleBeeInstance instance;
  auto out = RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  EXPECT_NE(out.find("Table created with id ="), std::string::npos);
  EXPECT_NE(instance.catalog_->GetTable("t1"), NULL_TABLE_INFO);
}

TEST(InstanceTest, CreateTableWithArrayColumns) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(id INT, tags INT[], fixed INT[3]);");
  auto table = instance.catalog_->GetTable("t1");
  ASSERT_NE(table, NULL_TABLE_INFO);
  EXPECT_EQ(table->schema_.GetColumn(1).GetType().ToString(), "INTEGER[]");
  EXPECT_EQ(table->schema_.GetColumn(2).GetType().ToString(), "INTEGER[3]");
}

TEST(InstanceTest, SelectPrintsTheOptimizedPlan) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  auto out = RunSql(instance, "SELECT * FROM t1 WHERE v1 = 1;");
  EXPECT_NE(out.find("=== OPTIMIZED PLAN (no execution engine yet) ==="), std::string::npos);
  // The filter has been folded into the scan.
  EXPECT_NE(out.find("SeqScan { table=t1, filter=(#0.0=1) }"), std::string::npos);
}

TEST(InstanceTest, ExplainShowsAllThreeStages) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  auto out = RunSql(instance, "EXPLAIN (binder,planner,optimizer) SELECT * FROM t1 WHERE v1 = 1;");

  const auto binder_at = out.find("=== BINDER ===");
  const auto planner_at = out.find("=== PLANNER ===");
  const auto optimizer_at = out.find("=== OPTIMIZER ===");
  ASSERT_NE(binder_at, std::string::npos);
  ASSERT_NE(planner_at, std::string::npos);
  ASSERT_NE(optimizer_at, std::string::npos);
  EXPECT_LT(binder_at, planner_at);
  EXPECT_LT(planner_at, optimizer_at);

  // The planner emits a separate Filter; the optimizer folds it into the scan.
  const auto planner_section = out.substr(planner_at, optimizer_at - planner_at);
  EXPECT_NE(planner_section.find("Filter"), std::string::npos);
  const auto optimizer_section = out.substr(optimizer_at);
  EXPECT_EQ(optimizer_section.find("Filter"), std::string::npos);
}

TEST(InstanceTest, TheWholePipelineOnOneQuery) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT, tags INT[]);");
  RunSql(instance, "CREATE TABLE t2(v3 INT, v4 INT);");
  auto out = RunSql(instance, "SELECT v1, v4 FROM t1, t2 WHERE v2 = v3 AND v1 > 10 ORDER BY v1 LIMIT 5;");

  // Sort + Limit collapsed into a TopN.
  EXPECT_NE(out.find("TopN { n=5"), std::string::npos);
  EXPECT_EQ(out.find("Sort {"), std::string::npos);
  // The equi-condition became a hash join...
  EXPECT_NE(out.find("HashJoin"), std::string::npos);
  EXPECT_EQ(out.find("NestedLoopJoin"), std::string::npos);
  // ...because the single-table conjunct was pushed into t1's scan first.
  EXPECT_NE(out.find("SeqScan { table=t1, filter=(#0.0>10) }"), std::string::npos);
  // And the array column survives the whole pipeline.
  EXPECT_NE(out.find("t1.tags:INTEGER[]"), std::string::npos);
}

TEST(InstanceTest, MetaCommands) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  auto out = RunSql(instance, "\\dt");
  EXPECT_NE(out.find("t1"), std::string::npos);
  EXPECT_NE(out.find("v1:INTEGER"), std::string::npos);

  EXPECT_NE(RunSql(instance, "\\help").find("BumbleBeeDB"), std::string::npos);
  EXPECT_THROW(RunSql(instance, "\\nonsense"), Exception);
}

TEST(InstanceTest, ErrorsAreThrownNotSwallowed) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  EXPECT_THROW(RunSql(instance, "SELECT * FROM nonexistent;"), Exception);
  EXPECT_THROW(RunSql(instance, "SELECT bogus FROM t1;"), Exception);
  EXPECT_THROW(RunSql(instance, "SELECT FROM FROM;"), Exception);
  // A duplicate CREATE must not silently overwrite the existing table.
  EXPECT_THROW(RunSql(instance, "CREATE TABLE t1(v1 INT);"), Exception);

  // The instance is still usable afterwards.
  EXPECT_NE(RunSql(instance, "SELECT * FROM t1;").find("SeqScan"), std::string::npos);
}

TEST(InstanceTest, MultipleStatementsInOneString) {
  BumbleBeeInstance instance;
  auto out = RunSql(instance, "CREATE TABLE t1(v1 INT); CREATE TABLE t2(v2 INT);");
  EXPECT_NE(instance.catalog_->GetTable("t1"), NULL_TABLE_INFO);
  EXPECT_NE(instance.catalog_->GetTable("t2"), NULL_TABLE_INFO);
}

}  // namespace bumblebee
