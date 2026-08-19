//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_grace_hash_join_test.cpp
//
// Identification: test/unit/execution/operator/join/physical_grace_hash_join_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "execution/operator/join/physical_grace_hash_join.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);

// Same fixture shape as the in-memory hash join test on purpose: the grace join's contract is
// "identical rows to PhysicalHashJoin", so the assertions are deliberately the same and only the
// operator under test differs. Sizes are chosen to spread rows across many of the
// GH_PARTITION_COUNT spill partitions rather than land in one.
auto LeftSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"l", kInt}}); }
auto RightSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"r", kInt}}); }
auto JoinSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"l", kInt}, {"k2", kInt}, {"r", kInt}}); }

auto Row(int32_t a, int32_t b) -> TestRow { return {Value(a), Value(b)}; }
auto NullKeyRow(int32_t b) -> TestRow { return {Value::Null(kInt), Value(b)}; }

/**
 * @brief Grace-join `left_rows` against `right_rows` on column 0 of each.
 *
 * `budget` bounds the query memory the join sees; the default is ample, and a small value forces
 * the oversized-pair repartition / block-nested-loop fallbacks in phase 3.
 */
auto RunJoin(const std::vector<TestRow> &left_rows, const std::vector<TestRow> &right_rows, JoinType join_type,
             idx_t chunk_size = 16, idx_t budget = MAX_MEMORY) -> std::vector<TestRow> {
  OperatorHarness h;
  h.client.mem_.SetBudget(budget);
  auto ls = LeftSchema();
  auto rs = RightSchema();
  auto left = std::make_unique<RowScan>(ls, left_rows, chunk_size);
  auto right = std::make_unique<RowScan>(rs, right_rows, chunk_size);

  std::vector<AbstractExpressionRef> left_keys{ColRef(ls, 0, 0)};
  std::vector<AbstractExpressionRef> right_keys{ColRef(rs, 0, 1)};

  auto join =
      std::make_unique<PhysicalGraceHashJoin>(JoinSchema(), std::move(left_keys), std::move(right_keys), join_type,
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

TEST(PhysicalGraceHashJoinTest, InnerJoinMatchesOnKey) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {Row(2, 200), Row(1, 100)}, JoinType::INNER);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|1|100", "2|20|2|200"}));
}

// Phase 3 claims (build, probe) partition pairs from a queue; empty partitions on either side must
// terminate cleanly rather than hang the source.
TEST(PhysicalGraceHashJoinTest, EmptyLeftInnerJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({}, {Row(1, 100), Row(2, 200)}, JoinType::INNER).empty());
}

TEST(PhysicalGraceHashJoinTest, EmptyRightInnerJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({Row(1, 10)}, {}, JoinType::INNER).empty());
}

TEST(PhysicalGraceHashJoinTest, EmptyRightLeftJoinNullPadsEveryLeftRow) {
  const auto rows = RunJoin({Row(1, 10), Row(2, 20)}, {}, JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|-|-", "2|20|-|-"}));
}

TEST(PhysicalGraceHashJoinTest, EmptyLeftLeftJoinProducesNoRows) {
  EXPECT_TRUE(RunJoin({}, {Row(1, 100)}, JoinType::LEFT).empty());
}

TEST(PhysicalGraceHashJoinTest, LeftJoinNullPadsUnmatchedLeftRows) {
  const auto rows = RunJoin({Row(1, 10), Row(9, 90)}, {Row(1, 100)}, JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|10|1|100", "9|90|-|-"}));
}

TEST(PhysicalGraceHashJoinTest, NullKeysNeverMatch) {
  const auto rows = RunJoin({NullKeyRow(10), Row(1, 11)}, {NullKeyRow(100), Row(1, 101)}, JoinType::INNER);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"1|11|1|101"})) << "NULL == NULL must not join";
}

// For LEFT the preserved probe side routes NULL-keyed rows to a dedicated spill during phase 2 and
// emits them NULL-padded — a code path the in-memory join does not have.
TEST(PhysicalGraceHashJoinTest, NullKeyOnPreservedSideIsNullPadded) {
  const auto rows = RunJoin({NullKeyRow(10), Row(1, 11)}, {Row(1, 101)}, JoinType::LEFT);
  EXPECT_EQ(Rendered(rows), (std::vector<std::string>{"-|10|-|-", "1|11|1|101"}));
}

TEST(PhysicalGraceHashJoinTest, DuplicateKeysProduceCartesianProductPerKey) {
  const auto rows = RunJoin({Row(1, 10), Row(1, 11)}, {Row(1, 100), Row(1, 101), Row(1, 102)}, JoinType::INNER);
  EXPECT_EQ(rows.size(), 6U);
}

// Enough distinct keys to populate many spill partitions, so the result is stitched together from
// many independent phase-3 pair joins — and it must equal the single-partition answer.
TEST(PhysicalGraceHashJoinTest, ManyPartitionsStitchBackTogether) {
  std::vector<TestRow> left;
  std::vector<TestRow> right;
  for (int i = 0; i < 500; i++) {
    left.push_back(Row(i, i * 10));
    right.push_back(Row(i % 250, i));  // every surviving key matches exactly twice
  }
  const auto rows = RunJoin(left, right, JoinType::INNER, 64);
  EXPECT_EQ(rows.size(), 500U) << "250 matched keys x 2 probe rows each";
}

// A tiny memory budget makes every partition pair look oversized, driving the phase-3 repartition
// path (and, for the single hot key, the block-nested-loop fallback that repartitioning can't split).
TEST(PhysicalGraceHashJoinTest, TinyBudgetStillJoinsCorrectly) {
  std::vector<TestRow> left;
  std::vector<TestRow> right;
  for (int i = 0; i < 200; i++) {
    left.push_back(Row(i, i * 10));
    right.push_back(Row(i, i * 100));
  }
  const auto ample = Rendered(RunJoin(left, right, JoinType::INNER, 64));
  const auto tiny = Rendered(RunJoin(left, right, JoinType::INNER, 64, /*budget=*/1));
  EXPECT_EQ(tiny, ample);
}

TEST(PhysicalGraceHashJoinTest, TinyBudgetHotKeyFallsBackToBlockNestedLoop) {
  std::vector<TestRow> left;
  std::vector<TestRow> right;
  for (int i = 0; i < 100; i++) {
    left.push_back(Row(7, i));  // one hot key: repartitioning can never split it
    right.push_back(Row(7, i));
  }
  const auto rows = RunJoin(left, right, JoinType::INNER, 16, /*budget=*/1);
  EXPECT_EQ(rows.size(), 10000U) << "100 x 100 rows on the single hot key";
}

TEST(PhysicalGraceHashJoinTest, ResultIsIndependentOfInputChunking) {
  std::vector<TestRow> left;
  std::vector<TestRow> right;
  for (int i = 0; i < 30; i++) {
    left.push_back(Row(i, i * 10));
    right.push_back(Row(i % 15, i * 100));  // half the probe keys match twice
  }
  const auto baseline = Rendered(RunJoin(left, right, JoinType::INNER, 64));
  for (const idx_t chunk : {idx_t{1}, idx_t{4}, idx_t{30}}) {
    EXPECT_EQ(Rendered(RunJoin(left, right, JoinType::INNER, chunk)), baseline) << "chunk size " << chunk;
  }
}

}  // namespace bumblebee
