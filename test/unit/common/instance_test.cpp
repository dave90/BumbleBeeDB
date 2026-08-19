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

#include <algorithm>
#include <chrono>  // NOLINT
#include <sstream>
#include <thread>  // NOLINT

#include "common/exception.h"
#include "gtest/gtest.h"

namespace bumblebee {

/** @brief Run a statement and return everything the instance wrote. */
static auto RunSql(BumbleBeeInstance &instance, const std::string &sql) -> std::string {
  std::stringstream ss;
  SimpleStreamWriter writer(ss);
  instance.ExecuteSql(sql, writer);
  return ss.str();
}

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
  // Column 0 is the auto "_id" primary key, so the declared columns start at index 1.
  EXPECT_EQ(table->schema_.GetColumn(0).GetName(), "_id");
  EXPECT_EQ(table->schema_.GetColumn(2).GetType().ToString(), "INTEGER[]");
  EXPECT_EQ(table->schema_.GetColumn(3).GetType().ToString(), "INTEGER[3]");
}

TEST(InstanceTest, SelectExecutesInsteadOfPrintingThePlan) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  auto out = RunSql(instance, "SELECT * FROM t1 WHERE v1 = 1;");
  // The execution engine now runs the query; the old "no execution engine yet" plan dump is gone.
  EXPECT_EQ(out.find("no execution engine yet"), std::string::npos);
  EXPECT_EQ(out.find("SeqScan"), std::string::npos);
  // The result header names the projected columns (no rows in an empty table).
  EXPECT_NE(out.find("v1"), std::string::npos);
  EXPECT_NE(out.find("v2"), std::string::npos);
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
  // Join/sort/limit operators are not lowered yet, so check the optimizer output via EXPLAIN.
  auto out = RunSql(
      instance, "EXPLAIN (optimizer,schema) SELECT v1, v4 FROM t1, t2 WHERE v2 = v3 AND v1 > 10 ORDER BY v1 LIMIT 5;");

  // Sort + Limit collapsed into a TopN.
  EXPECT_NE(out.find("TopN { n=5"), std::string::npos);
  EXPECT_EQ(out.find("Sort {"), std::string::npos);
  // The equi-condition became a hash join...
  EXPECT_NE(out.find("HashJoin"), std::string::npos);
  EXPECT_EQ(out.find("NestedLoopJoin"), std::string::npos);
  // ...because the single-table conjunct was pushed into t1's scan first. v1 is column 1 (column 0 is
  // the auto "_id" primary key), so the pushed filter references #0.1. Column pruning then noted
  // which columns the query actually reads (v1 for the filter/output and v2 for the join key).
  EXPECT_NE(out.find("SeqScan { table=t1, filter=(#0.1>10), columns=[1, 2] }"), std::string::npos);
  // And the array column survives the whole pipeline.
  EXPECT_NE(out.find("t1.tags:INTEGER[]"), std::string::npos);
}

TEST(InstanceTest, ExplainPhysicalAndPipelines) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  auto out = RunSql(instance, "EXPLAIN (physical,pipelines) SELECT v1 FROM t1 WHERE v1 > 5;");

  const auto physical_at = out.find("=== PHYSICAL ===");
  const auto pipelines_at = out.find("=== PIPELINES ===");
  ASSERT_NE(physical_at, std::string::npos);
  ASSERT_NE(pipelines_at, std::string::npos);
  EXPECT_LT(physical_at, pipelines_at);
  // The physical tree names the concrete operators and the row-format scan.
  EXPECT_NE(out.find("ResultCollector"), std::string::npos);
  EXPECT_NE(out.find("TableScan { table=t1, storage=row }"), std::string::npos);
  // The pipeline dump names the source and sink roles.
  EXPECT_NE(out.find("source"), std::string::npos);
  EXPECT_NE(out.find("sink"), std::string::npos);
}

TEST(InstanceTest, ExplainAnalyzeRunsAndReportsRows) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t1(v1 INT, v2 INT);");
  RunSql(instance, "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);");
  auto out = RunSql(instance, "EXPLAIN (analyze) SELECT COUNT(*) FROM t1;");
  EXPECT_NE(out.find("=== ANALYZE ==="), std::string::npos);
  EXPECT_NE(out.find("rows=3"), std::string::npos);  // the scan produced 3 rows
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

  // The instance is still usable afterwards: the query executes without error.
  EXPECT_NO_THROW(RunSql(instance, "SELECT * FROM t1;"));
}

TEST(InstanceTest, MultipleStatementsInOneString) {
  BumbleBeeInstance instance;
  auto out = RunSql(instance, "CREATE TABLE t1(v1 INT); CREATE TABLE t2(v2 INT);");
  EXPECT_NE(instance.catalog_->GetTable("t1"), NULL_TABLE_INFO);
  EXPECT_NE(instance.catalog_->GetTable("t2"), NULL_TABLE_INFO);
}

TEST(InstanceTest, DisplayRowCapTruncatesWithNotice) {
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE big(x INT);");
  RunSql(instance, "INSERT INTO big VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);");  // 10 rows

  // Run `SELECT x FROM big` through a stream writer capped at `n` rows (0 = unlimited).
  auto run_capped = [&](idx_t n) {
    std::stringstream ss;
    SimpleStreamWriter writer(ss, /*disable_header=*/false, /*separator=*/"\t", /*max_display_rows=*/n);
    instance.ExecuteSql("SELECT x FROM big;", writer);
    return ss.str();
  };
  // Output is one header line + one line per emitted row + (if truncated) one notice line.
  auto lines = [](const std::string &s) { return std::count(s.begin(), s.end(), '\n'); };

  // Cap below the row count: only 3 rows are emitted, and the notice reports the TRUE total (10).
  auto capped = run_capped(3);
  EXPECT_NE(capped.find("-- showing first 3 of 10 rows"), std::string::npos);
  EXPECT_EQ(lines(capped), 5) << "header + 3 rows + notice";

  // Unlimited (0): every row, no notice.
  auto unlimited = run_capped(0);
  EXPECT_EQ(unlimited.find("showing first"), std::string::npos);
  EXPECT_EQ(lines(unlimited), 11) << "header + 10 rows";

  // Cap at or above the row count: nothing is truncated, so no notice.
  auto under_cap = run_capped(20);
  EXPECT_EQ(under_cap.find("showing first"), std::string::npos);
  EXPECT_EQ(lines(under_cap), 11) << "header + 10 rows";
}

TEST(InstanceTest, StringVectorWriterIsUnlimitedByDefault) {
  // Machine consumers (tests, the e2e protocol) must never be truncated: their writers report 0.
  StringVectorWriter collector;
  NoopWriter noop;
  EXPECT_EQ(collector.MaxDisplayRows(), 0U);
  EXPECT_EQ(noop.MaxDisplayRows(), 0U);
}

TEST(InstanceTest, SessionsHoldIndependentTransactions) {
  // `\session <name>` switches which named session subsequent statements run in; each session can
  // hold its own open transaction, so MVCC visibility applies between them within one instance.
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t(v INT);");

  RunSql(instance, "\\session s1");
  RunSql(instance, "BEGIN;");
  RunSql(instance, "INSERT INTO t VALUES (1);");
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('1'), std::string::npos)
      << "s1 sees its own uncommitted insert";

  RunSql(instance, "\\session s2");
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('0'), std::string::npos)
      << "s2 must not see s1's uncommitted insert";
  // s2 was left in autocommit by the switch, so it can open its own transaction.
  RunSql(instance, "BEGIN;");

  RunSql(instance, "\\session s1");
  RunSql(instance, "COMMIT;");

  RunSql(instance, "\\session s2");
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('0'), std::string::npos)
      << "s2's snapshot predates s1's commit";
  RunSql(instance, "COMMIT;");
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('1'), std::string::npos)
      << "a fresh s2 statement sees the committed row";

  // `\session` is silent (no output), so the e2e harness can inject switches freely.
  EXPECT_EQ(RunSql(instance, "\\session s3"), "");
}

TEST(InstanceTest, GcAbortsTimedOutTransactionAndSessionRecovers) {
  // A millisecond timeout (runtime ctor parameter — no special build needed) makes the next `\gc`
  // pass abort the open transaction; its writes are rolled back and the session drops back to
  // autocommit, so COMMIT then has nothing to commit and later transactions work normally.
  BumbleBeeInstance instance(std::chrono::milliseconds(20));
  RunSql(instance, "CREATE TABLE t(v INT);");
  RunSql(instance, "BEGIN;");
  RunSql(instance, "INSERT INTO t VALUES (1);");

  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  auto gc_out = RunSql(instance, "\\gc");
  EXPECT_NE(gc_out.find("aborted 1 timed-out"), std::string::npos) << gc_out;

  EXPECT_THROW(RunSql(instance, "COMMIT;"), Exception) << "the timed-out transaction is gone";
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('0'), std::string::npos)
      << "the aborted transaction's insert was rolled back";

  // The same session opens a fresh transaction afterwards, unaffected by the reaped one.
  RunSql(instance, "BEGIN;");
  RunSql(instance, "INSERT INTO t VALUES (2);");
  RunSql(instance, "COMMIT;");
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('1'), std::string::npos);
}

TEST(InstanceTest, GcLeavesYoungTransactionsAlone) {
  // With the default (2h) timeout nothing expires: \gc reports zero aborts and an open transaction
  // keeps working across the pass.
  BumbleBeeInstance instance;
  RunSql(instance, "CREATE TABLE t(v INT);");
  RunSql(instance, "BEGIN;");
  RunSql(instance, "INSERT INTO t VALUES (1);");
  auto gc_out = RunSql(instance, "\\gc");
  EXPECT_NE(gc_out.find("aborted 0 timed-out"), std::string::npos) << gc_out;
  RunSql(instance, "COMMIT;");
  EXPECT_NE(RunSql(instance, "SELECT COUNT(*) FROM t;").find('1'), std::string::npos);
}

}  // namespace bumblebee
