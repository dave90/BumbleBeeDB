//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column_pruning_test.cpp
//
// Identification: test/unit/optimizer/column_pruning_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <gtest/gtest.h>

#include <sstream>

#include "bumblebee_instance.h"

namespace bumblebee {

/** The column-pruning pass: which columns each scan is told to materialize, asserted through the
 * optimizer's EXPLAIN rendering (`columns=[...]` on pruned SeqScans). */
class ColumnPruningTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Column layout of t (auto _id first): _id=0, a=1, b=2, c=3, d=4.
    Run("CREATE TABLE t(a INT, b VARCHAR, c DOUBLE, d BIGINT);");
    Run("CREATE TABLE u(k INT, v VARCHAR);");  // _id=0, k=1, v=2
  }

  auto Run(const std::string &sql) -> std::string {
    std::stringstream ss;
    SimpleStreamWriter writer(ss);
    instance_.ExecuteSql(sql, writer);
    return ss.str();
  }

  auto Explain(const std::string &query) -> std::string { return Run("EXPLAIN (optimizer) " + query); }

  BumbleBeeInstance instance_;
};

TEST_F(ColumnPruningTest, ScanKeepsOnlyReadColumns) {
  // The output column and the filter column survive; _id, b, c are pruned.
  auto out = Explain("SELECT a FROM t WHERE d > 15;");
  EXPECT_NE(out.find("columns=[1, 4]"), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, SelectStarPrunesNothing) {
  auto out = Explain("SELECT * FROM t;");
  EXPECT_EQ(out.find("columns=["), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, CountStarKeepsOneColumnAlive) {
  // No column is referenced at all; the scan keeps a single column (the backends cannot express
  // a zero-column scan).
  auto out = Explain("SELECT COUNT(*) FROM t;");
  EXPECT_NE(out.find("columns=[0]"), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, AggregationRequiresKeysAndArguments) {
  // GROUP BY a, SUM(d): exactly those two columns.
  auto out = Explain("SELECT a, SUM(d) FROM t GROUP BY a;");
  EXPECT_NE(out.find("columns=[1, 4]"), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, JoinKeepsKeysAndProjectedColumnsPerSide) {
  auto out = Explain("SELECT t.b FROM t, u WHERE t.a = u.k;");
  // t: b (projected) + a (join key). u: k (join key) only.
  EXPECT_NE(out.find("columns=[1, 2]"), std::string::npos) << out;
  EXPECT_NE(out.find("columns=[1]"), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, OrderByColumnsCountAsRead) {
  // ORDER BY must name select-list columns in this engine; the TopN keys flow through the
  // projection into the scan requirement.
  auto out = Explain("SELECT a, d FROM t ORDER BY d LIMIT 3;");
  EXPECT_NE(out.find("columns=[1, 4]"), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, DmlChildrenAreNeverPruned) {
  Run("INSERT INTO t VALUES (1,'x',1.5,10);");
  auto out = Explain("UPDATE t SET a = 2 WHERE d > 5;");
  EXPECT_EQ(out.find("columns=["), std::string::npos) << out;
  out = Explain("DELETE FROM t WHERE a = 1;");
  EXPECT_EQ(out.find("columns=["), std::string::npos) << out;
}

TEST_F(ColumnPruningTest, PrunedQueriesReturnCorrectResults) {
  Run("INSERT INTO t VALUES (1,'x',1.5,10),(2,'y',2.5,20),(3,'z',3.5,30);");
  Run("INSERT INTO u VALUES (2,'two'),(3,'three'),(4,'four');");
  EXPECT_EQ(Run("SELECT a FROM t WHERE d > 15 ORDER BY a;"), "t.a\t\n2\t\n3\t\n");
  EXPECT_EQ(Run("SELECT COUNT(*) FROM t;"), "__unnamed#0\t\n3\t\n");
  // Join with per-side pruning: values must come through untouched.
  EXPECT_EQ(Run("SELECT t.b FROM t, u WHERE t.a = u.k ORDER BY t.b;"), "t.b\t\n'y'\t\n'z'\t\n");
  // A subquery output nobody reads: its inputs may be pruned, the read one must be exact.
  EXPECT_EQ(Run("SELECT x FROM (SELECT a AS x, c AS y FROM t) s WHERE x = 2;"), "s.x\t\n2\t\n");
}

}  // namespace bumblebee
