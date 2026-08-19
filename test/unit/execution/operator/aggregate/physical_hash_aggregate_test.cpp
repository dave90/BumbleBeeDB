//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_hash_aggregate_test.cpp
//
// Identification: test/unit/execution/operator/aggregate/physical_hash_aggregate_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "execution/operator/aggregate/physical_hash_aggregate.h"
#include "execution/plans/aggregation_plan.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);
const LogicalType kStr(LogicalTypeId::STRING);

/** Input rows are (g INTEGER group key, v INTEGER value, s VARCHAR value). */
auto InputSchema() -> SchemaRef { return MakeSchemaOf({{"g", kInt}, {"v", kInt}, {"s", kStr}}); }

auto Row(int32_t g, int32_t v, const std::string &s) -> TestRow { return {Value(g), Value(v), Value(s)}; }
auto NullValueRow(int32_t g, const std::string &s) -> TestRow { return {Value(g), Value::Null(kInt), Value(s)}; }
auto NullGroupRow(int32_t v, const std::string &s) -> TestRow { return {Value::Null(kInt), Value(v), Value(s)}; }

/**
 * @brief GROUP BY column 0, applying `agg_types` to `arg_col` (one aggregate per type).
 *
 * The output schema is the group column then one column per aggregate, typed by the engine's own
 * `AggResultType` so the test cannot disagree with the operator about result typing.
 */
auto RunAgg(const std::vector<TestRow> &rows, const std::vector<AggregationType> &agg_types, uint32_t arg_col,
            idx_t chunk_size = 16) -> std::vector<TestRow> {
  OperatorHarness h;
  auto in = InputSchema();

  std::vector<Column> out_cols{in->GetColumn(0)};
  std::vector<AbstractExpressionRef> aggregates;
  for (size_t a = 0; a < agg_types.size(); a++) {
    const auto res = AggregationPlanNode::AggResultType(agg_types[a], in->GetColumn(arg_col).GetType());
    out_cols.emplace_back(res.GetTypeId() == LogicalTypeId::STRING
                              ? Column("a" + std::to_string(a), res, VARCHAR_DEFAULT_LENGTH)
                              : Column("a" + std::to_string(a), res));
    aggregates.push_back(ColRef(in, arg_col));
  }
  auto out_schema = std::make_shared<Schema>(out_cols);

  std::vector<AbstractExpressionRef> group_bys{ColRef(in, 0)};
  auto scan = std::make_unique<RowScan>(in, rows, chunk_size);
  auto agg = std::make_unique<PhysicalHashAggregate>(out_schema, std::move(group_bys), std::move(aggregates), agg_types,
                                                     std::move(scan));
  RowCollector collector(std::move(agg));
  auto out = h.Run(collector);
  SortRows(out);
  return out;
}

/** @brief One value as plain text: NULL as "-", and strings unquoted (Value::ToString quotes them). */
auto Text(const Value &v) -> std::string {
  if (v.IsNull()) {
    return "-";
  }
  return v.GetPhysicalType() == PhysicalType::STRING ? v.GetString() : v.ToString();
}

/** @brief Render a result set as "group => a0 a1 ..." strings. */
auto Rendered(const std::vector<TestRow> &rows) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const auto &r : rows) {
    std::string s = Text(r[0]) + " =>";
    for (size_t i = 1; i < r.size(); i++) {
      s += " " + Text(r[i]);
    }
    out.push_back(s);
  }
  return out;
}

}  // namespace

TEST(PhysicalHashAggregateTest, GroupsAndCountsSumsMinMaxes) {
  const std::vector<TestRow> rows{Row(1, 10, "b"), Row(1, 30, "a"), Row(2, 7, "z")};
  const auto out = RunAgg(rows,
                          {AggregationType::CountAggregate, AggregationType::SumAggregate,
                           AggregationType::MinAggregate, AggregationType::MaxAggregate},
                          /*arg_col=*/1);
  EXPECT_EQ(Rendered(out), (std::vector<std::string>{"1 => 2 40 10 30", "2 => 1 7 7 7"}));
}

TEST(PhysicalHashAggregateTest, AvgDividesSumByCount) {
  const std::vector<TestRow> rows{Row(1, 10, "a"), Row(1, 20, "a"), Row(1, 30, "a")};
  const auto out = RunAgg(rows, {AggregationType::AvgAggregate}, 1);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_DOUBLE_EQ(out[0][1].GetAs<double>(), 20.0);
}

// A grouped aggregate over no input produces no groups at all — not one empty group. (The
// whole-table aggregate, which does produce a row, is PhysicalUngroupedAggregate.)
TEST(PhysicalHashAggregateTest, EmptyInputProducesNoGroups) {
  EXPECT_TRUE(RunAgg({}, {AggregationType::CountAggregate}, 1).empty());
}

// SQL GROUP BY treats NULL as its own group — unlike join equality, where NULL matches nothing.
// The group hash table is built with null_equal_keys = true precisely for this.
TEST(PhysicalHashAggregateTest, NullGroupKeyFormsItsOwnGroup) {
  const std::vector<TestRow> rows{NullGroupRow(1, "a"), NullGroupRow(2, "a"), Row(5, 3, "a")};
  const auto out = RunAgg(rows, {AggregationType::CountAggregate, AggregationType::SumAggregate}, 1);
  EXPECT_EQ(Rendered(out), (std::vector<std::string>{"- => 2 3", "5 => 1 3"}));
}

// COUNT(x) skips NULL arguments; SUM/MIN/MAX ignore them too but stay non-NULL while some value
// remains. This is the "state initialized == count > 0" convention in the row layout.
TEST(PhysicalHashAggregateTest, NullArgumentsAreSkippedNotCounted) {
  const std::vector<TestRow> rows{Row(1, 5, "a"), NullValueRow(1, "a"), Row(1, 7, "a")};
  const auto out = RunAgg(rows,
                          {AggregationType::CountAggregate, AggregationType::SumAggregate,
                           AggregationType::MinAggregate, AggregationType::MaxAggregate},
                          1);
  EXPECT_EQ(Rendered(out), (std::vector<std::string>{"1 => 2 12 5 7"}));
}

// A group whose argument is NULL in every row: COUNT is 0 and the value aggregates are NULL,
// because no state was ever initialised.
TEST(PhysicalHashAggregateTest, AllNullGroupYieldsZeroCountAndNullValues) {
  const std::vector<TestRow> rows{NullValueRow(1, "a"), NullValueRow(1, "a")};
  const auto out = RunAgg(rows,
                          {AggregationType::CountAggregate, AggregationType::SumAggregate,
                           AggregationType::MinAggregate, AggregationType::MaxAggregate},
                          1);
  EXPECT_EQ(Rendered(out), (std::vector<std::string>{"1 => 0 - - -"}));
}

// String MIN/MAX keeps its running extreme OFF the fixed-width row (the slot holds a 1-based index
// into the table's own string store). That off-row path is separate from the numeric one and is
// only reachable with a VARCHAR argument.
TEST(PhysicalHashAggregateTest, StringMinMaxComparesLexicographically) {
  const std::vector<TestRow> rows{Row(1, 0, "pear"), Row(1, 0, "apple"), Row(1, 0, "fig"), Row(2, 0, "solo")};
  const auto out = RunAgg(rows, {AggregationType::MinAggregate, AggregationType::MaxAggregate}, /*arg_col=*/2);
  EXPECT_EQ(Rendered(out), (std::vector<std::string>{"1 => apple pear", "2 => solo solo"}));
}

TEST(PhysicalHashAggregateTest, StringMinMaxHandlesEmptyAndLongValues) {
  const std::string long_value(200, 'z');  // forces the off-row string store, not the inline prefix
  const std::vector<TestRow> rows{Row(1, 0, ""), Row(1, 0, long_value), Row(1, 0, "m")};
  const auto out = RunAgg(rows, {AggregationType::MinAggregate, AggregationType::MaxAggregate}, 2);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0][1].GetString(), "") << "the empty string is the lexicographic minimum";
  EXPECT_EQ(out[0][2].GetString(), long_value);
}

// Chunking changes how rows are distributed across sink batches and therefore how partial states
// are merged. The answer must not depend on it.
TEST(PhysicalHashAggregateTest, ResultIsIndependentOfInputChunking) {
  std::vector<TestRow> rows;
  rows.reserve(120);
  for (int i = 0; i < 120; i++) {
    rows.push_back(Row(i % 7, i, "s" + std::to_string(i % 7)));
  }
  const auto baseline = Rendered(RunAgg(rows,
                                        {AggregationType::CountAggregate, AggregationType::SumAggregate,
                                         AggregationType::MinAggregate, AggregationType::MaxAggregate},
                                        1, 256));
  for (const idx_t chunk : {idx_t{1}, idx_t{5}, idx_t{120}}) {
    EXPECT_EQ(Rendered(RunAgg(rows,
                              {AggregationType::CountAggregate, AggregationType::SumAggregate,
                               AggregationType::MinAggregate, AggregationType::MaxAggregate},
                              1, chunk)),
              baseline)
        << "chunk size " << chunk;
  }
}

// Enough distinct groups to exercise the partitioned sink and the partition-wise merge, rather
// than the single-table fast path.
TEST(PhysicalHashAggregateTest, ManyDistinctGroupsAggregateCorrectly) {
  constexpr int kGroups = 500;
  std::vector<TestRow> rows;
  rows.reserve(kGroups * 2);
  for (int i = 0; i < kGroups; i++) {
    rows.push_back(Row(i, 1, "s"));
    rows.push_back(Row(i, 2, "s"));
  }
  const auto out = RunAgg(rows, {AggregationType::CountAggregate, AggregationType::SumAggregate}, 1, 64);
  ASSERT_EQ(out.size(), static_cast<size_t>(kGroups));
  for (const auto &r : out) {
    EXPECT_EQ(r[1].GetAs<int32_t>(), 2) << "group " << r[0].ToString();
    EXPECT_EQ(r[2].GetAs<int64_t>(), 3) << "group " << r[0].ToString();
  }
}

}  // namespace bumblebee
