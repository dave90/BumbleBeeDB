//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_hash_join_test.cpp
//
// Identification: test/unit/execution/operator/join/physical_hash_join_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "execution/operator/join/physical_hash_join.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);

// Left input (k, l) and right input (k, r); the join output is all four columns.
//
// The operator's contract: the last two constructor parameters are child 0 = LEFT and child 1 =
// RIGHT. Which one is hashed is decided by the join type — `BuildChildIdx()` returns
// `PreservesLeft() ? 1 : 0` — so for LEFT, SEMI and ANTI the *second* argument builds and the
// first streams through as the probe. (They were once named `build, probe`, which said the
// opposite and cost this file a wrong assertion before the rename.)
auto LeftSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"l", kInt}}); }
auto RightSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"r", kInt}}); }
auto JoinSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"l", kInt}, {"k2", kInt}, {"r", kInt}}); }

auto Row(int32_t a, int32_t b) -> TestRow { return {Value(a), Value(b)}; }
auto NullKeyRow(int32_t b) -> TestRow { return {Value::Null(kInt), Value(b)}; }

/**
 * @brief Join `left_rows` against `right_rows` on column 0 of each.
 *
 * `chunk_size` splits both inputs, so the same case can be run single-chunk and multi-chunk.
 */
auto RunJoin(const std::vector<TestRow> &left_rows, const std::vector<TestRow> &right_rows, JoinType join_type,
             idx_t chunk_size = 16) -> std::vector<TestRow> {
  OperatorHarness h;
  auto ls = LeftSchema();
  auto rs = RightSchema();
  auto left = std::make_unique<RowScan>(ls, left_rows, chunk_size);
  auto right = std::make_unique<RowScan>(rs, right_rows, chunk_size);

  std::vector<AbstractExpressionRef> left_keys{ColRef(ls, 0, 0)};
  std::vector<AbstractExpressionRef> right_keys{ColRef(rs, 0, 1)};

  auto join = std::make_unique<PhysicalHashJoin>(JoinSchema(), std::move(left_keys), std::move(right_keys), join_type,
                                                 /*left_column_count=*/2, std::move(left), std::move(right));
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

TEST(PhysicalHashJoinTest, InnerJoinMatchesOnKey) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {Row(2, 200), Row(1, 100)}, JoinType::INNER);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|1|100", "2|20|2|200"}));
}

// An input that produces no rows at all: the other side must still run to completion and emit
// nothing (INNER) rather than deadlocking or emitting garbage. This is the case e2e reaches only
// via a query whose subplan happens to be empty.
TEST(PhysicalHashJoinTest, EmptyLeftInnerJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({}, {Row(1, 100), Row(2, 200)}, JoinType::INNER).empty());
}

TEST(PhysicalHashJoinTest, EmptyRightInnerJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({Row(1, 10)}, {}, JoinType::INNER).empty());
}

// LEFT JOIN preserves the LEFT input, so an empty right side NULL-pads every left row.
TEST(PhysicalHashJoinTest, EmptyRightLeftJoinNullPadsEveryLeftRow) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {}, JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|-|-", "2|20|-|-"}));
}

// ...and an empty LEFT input yields nothing at all, because there is no row to preserve.
TEST(PhysicalHashJoinTest, EmptyLeftLeftJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({}, {Row(1, 100)}, JoinType::LEFT).empty());
}

TEST(PhysicalHashJoinTest, LeftJoinNullPadsUnmatchedLeftRows) {
  const auto rows = RunJoin({Row(1, 10), Row(9, 90)}, {Row(1, 100)}, JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|1|100", "9|90|-|-"}));
}

// SQL equality is never true for NULL, so a NULL key must not match anything — not even another
// NULL. Getting this wrong is invisible in most queries but silently wrong in the rest.
TEST(PhysicalHashJoinTest, NullKeysNeverMatch) {
  const auto rows = RunJoin({NullKeyRow(10), Row(1, 11)}, {NullKeyRow(100), Row(1, 101)}, JoinType::INNER);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|11|1|101"})) << "NULL == NULL must not join";
}

// A LEFT row whose key is NULL matches nothing, so LEFT JOIN preserves it NULL-padded.
TEST(PhysicalHashJoinTest, NullKeyOnPreservedSideIsNullPadded) {
  const auto rows = RunJoin({NullKeyRow(10)}, {Row(1, 100)}, JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"-|10|-|-"}));
}

// Many-to-many: 2 build rows × 3 probe rows on the same key is 6 output rows. A bucket-chain bug
// typically shows up here as a dropped or duplicated row.
TEST(PhysicalHashJoinTest, DuplicateKeysProduceCartesianProductPerKey) {
  const auto rows = RunJoin({Row(1, 10), Row(1, 11)}, {Row(1, 100), Row(1, 101), Row(1, 102)}, JoinType::INNER);
  EXPECT_EQ(rows.size(), 6u);
}

// Splitting either side across chunks must not change the join result.
TEST(PhysicalHashJoinTest, ResultIsIndependentOfInputChunking) {
  std::vector<TestRow> build;
  std::vector<TestRow> probe;
  for (int i = 0; i < 30; i++) {
    build.push_back(Row(i, i * 10));
    probe.push_back(Row(i % 15, i * 100));  // half the probe keys match twice
  }
  const auto baseline = Rendered(RunJoin(build, probe, JoinType::INNER, 64));
  for (const idx_t chunk : {idx_t{1}, idx_t{4}, idx_t{30}}) {
    EXPECT_EQ(Rendered(RunJoin(build, probe, JoinType::INNER, chunk)), baseline) << "chunk size " << chunk;
  }
}

}  // namespace bumblebee
