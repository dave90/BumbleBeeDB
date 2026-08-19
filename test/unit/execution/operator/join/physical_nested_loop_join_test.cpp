//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_nested_loop_join_test.cpp
//
// Identification: test/unit/execution/operator/join/physical_nested_loop_join_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/operator/join/physical_nested_loop_join.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);

// Outer input (k, l) and inner input (k, r); output is left ++ right. Unlike the hash join, the
// OUTER side is child 0 and streams while the INNER side (child 1) is materialized in the sink —
// so LEFT preserves the *streaming* side here, the mirror image of the hash join's build/probe.
auto LeftSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"l", kInt}}); }
auto RightSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"r", kInt}}); }
auto JoinSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"l", kInt}, {"k2", kInt}, {"r", kInt}}); }

auto Row(int32_t a, int32_t b) -> TestRow { return {Value(a), Value(b)}; }
auto NullKeyRow(int32_t b) -> TestRow { return {Value::Null(kInt), Value(b)}; }

/** @brief `left.col0 <cmp> right.col0`, the two-sided predicate shape the planner emits. */
auto ColPredicate(ComparisonType cmp) -> AbstractExpressionRef {
  auto ls = LeftSchema();
  auto rs = RightSchema();
  return std::make_shared<ComparisonExpression>(ColRef(ls, 0, 0), ColRef(rs, 0, 1), cmp);
}

/** @brief The constant-TRUE predicate the planner uses for a cross product. */
auto TruePredicate() -> AbstractExpressionRef { return std::make_shared<ConstantValueExpression>(Value(true)); }

auto RunJoin(const std::vector<TestRow> &outer_rows, const std::vector<TestRow> &inner_rows,
             AbstractExpressionRef predicate, JoinType join_type, idx_t chunk_size = 16) -> std::vector<TestRow> {
  OperatorHarness h;
  auto outer = std::make_unique<RowScan>(LeftSchema(), outer_rows, chunk_size);
  auto inner = std::make_unique<RowScan>(RightSchema(), inner_rows, chunk_size);
  auto join = std::make_unique<PhysicalNestedLoopJoin>(JoinSchema(), std::move(predicate), join_type,
                                                       /*left_column_count=*/2, std::move(outer), std::move(inner));
  RowCollector collector(std::move(join));
  auto rows = h.Run(collector);
  SortRows(rows);
  return rows;
}

/** @brief Render rows as "k|l|k2|r" strings, with NULL shown as "-", for readable assertions. */
auto Rendered(const std::vector<TestRow> &rows) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const auto &r : rows) {
    std::string s;
    for (size_t i = 0; i < r.size(); i++) {
      s += (i != 0 ? "|" : "");
      s += r[i].IsNull() ? "-" : r[i].ToString();
    }
    out.push_back(s);
  }
  return out;
}

}  // namespace

TEST(PhysicalNestedLoopJoinTest, InnerEquiJoinMatchesOnKey) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {Row(2, 200), Row(1, 100)}, ColPredicate(ComparisonType::Equal),
                            JoinType::INNER);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|1|100", "2|20|2|200"}));
}

// The reason this operator exists: a predicate a hash join cannot evaluate.
TEST(PhysicalNestedLoopJoinTest, NonEquiPredicateJoinsAllLesserPairs) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {Row(1, 100), Row(2, 200), Row(3, 300)},
                            ColPredicate(ComparisonType::LessThan), JoinType::INNER);
  // 1 < {2,3} and 2 < {3}: three pairs.
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|2|200", "1|10|3|300", "2|20|3|300"}));
}

TEST(PhysicalNestedLoopJoinTest, ConstantTruePredicateIsCrossProduct) {
  const auto rows =
      RunJoin({Row(1, 10), Row(2, 20)}, {Row(7, 70), Row(8, 80), Row(9, 90)}, TruePredicate(), JoinType::INNER);
  EXPECT_EQ(rows.size(), 6U) << "2 x 3 cross product";
}

TEST(PhysicalNestedLoopJoinTest, EmptyOuterInnerJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({}, {Row(1, 100)}, ColPredicate(ComparisonType::Equal), JoinType::INNER).empty());
}

TEST(PhysicalNestedLoopJoinTest, EmptyInnerInnerJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({Row(1, 10)}, {}, ColPredicate(ComparisonType::Equal), JoinType::INNER).empty());
}

// LEFT preserves the outer (streaming) side: an empty inner side NULL-pads every outer row.
TEST(PhysicalNestedLoopJoinTest, EmptyInnerLeftJoinNullPadsEveryOuterRow) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {}, ColPredicate(ComparisonType::Equal), JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|-|-", "2|20|-|-"}));
}

TEST(PhysicalNestedLoopJoinTest, LeftJoinNullPadsUnmatchedOuterRows) {
  const auto rows =
      RunJoin({Row(1, 10), Row(9, 90)}, {Row(1, 100)}, ColPredicate(ComparisonType::Equal), JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|1|100", "9|90|-|-"}));
}

// A LEFT row must be padded exactly once even when the inner side spans several chunks — the
// "matched nothing" decision is only final after the LAST inner chunk, which is exactly the
// state machine the (inner chunk, match cursor) re-entrancy has to get right.
TEST(PhysicalNestedLoopJoinTest, LeftJoinPadsOnceAcrossManyInnerChunks) {
  std::vector<TestRow> inner;
  for (int i = 0; i < 30; i++) {
    inner.push_back(Row(100 + i, i));  // no inner key ever equals 1
  }
  const auto rows = RunJoin({Row(1, 10)}, inner, ColPredicate(ComparisonType::Equal), JoinType::LEFT, 4);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|-|-"}));
}

// SQL comparison with a NULL operand is never true, so NULL keys join nothing (and are LEFT-padded).
TEST(PhysicalNestedLoopJoinTest, NullKeysNeverSatisfyThePredicate) {
  const auto inner_rows = RunJoin({NullKeyRow(10), Row(1, 11)}, {NullKeyRow(100), Row(1, 101)},
                                  ColPredicate(ComparisonType::Equal), JoinType::INNER);
  EXPECT_EQ(Rendered(inner_rows), (std::vector<std::string>{"1|11|1|101"})) << "NULL == NULL must not join";

  const auto left_rows = RunJoin({NullKeyRow(10)}, {Row(1, 100)}, ColPredicate(ComparisonType::Equal), JoinType::LEFT);
  EXPECT_EQ(Rendered(left_rows), (std::vector<std::string>{"-|10|-|-"}));
}

TEST(PhysicalNestedLoopJoinTest, DuplicateMatchesProduceCartesianProductPerKey) {
  const auto rows = RunJoin({Row(1, 10), Row(1, 11)}, {Row(1, 100), Row(1, 101), Row(1, 102)},
                            ColPredicate(ComparisonType::Equal), JoinType::INNER);
  EXPECT_EQ(rows.size(), 6U);
}

// Splitting either side across chunks must not change the result — the outer side re-enters
// Execute per chunk and the inner side is rebuilt from several sink calls.
TEST(PhysicalNestedLoopJoinTest, ResultIsIndependentOfInputChunking) {
  std::vector<TestRow> outer;
  std::vector<TestRow> inner;
  for (int i = 0; i < 30; i++) {
    outer.push_back(Row(i, i * 10));
    inner.push_back(Row(i % 15, i * 100));  // half the keys match twice
  }
  const auto baseline = Rendered(RunJoin(outer, inner, ColPredicate(ComparisonType::Equal), JoinType::INNER, 64));
  for (const idx_t chunk : {idx_t{1}, idx_t{4}, idx_t{30}}) {
    EXPECT_EQ(Rendered(RunJoin(outer, inner, ColPredicate(ComparisonType::Equal), JoinType::INNER, chunk)), baseline)
        << "chunk size " << chunk;
  }
}

}  // namespace bumblebee
