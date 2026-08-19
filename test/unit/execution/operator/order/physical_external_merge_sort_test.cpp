//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_external_merge_sort_test.cpp
//
// Identification: test/unit/execution/operator/order/physical_external_merge_sort_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "execution/operator/order/physical_external_merge_sort.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);
const LogicalType kStr(LogicalTypeId::STRING);

auto TwoColSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"s", kStr}}); }

auto OrderOn(const SchemaRef &schema, uint32_t col, OrderByType dir) -> std::vector<OrderBy> {
  std::vector<OrderBy> obs;
  obs.emplace_back(dir, OrderByNullType::DEFAULT, ColRef(schema, col));
  return obs;
}

auto KeyColumn(const std::vector<TestRow> &rows) -> std::vector<int32_t> {
  std::vector<int32_t> out;
  out.reserve(rows.size());
  for (const auto &r : rows) {
    out.push_back(r[0].IsNull() ? INT32_MIN : r[0].GetAs<int32_t>());
  }
  return out;
}

/**
 * @brief Run the external sort over `rows`, `chunk_size` rows per input chunk.
 *
 * The contract is "identical rows, identical order to PhysicalSort", so the assertions mirror the
 * PhysicalSort tests. `budget` is the query memory budget: the sink flushes a sorted run whenever
 * the next reservation would exceed it, so a budget of 0 forces a run per input chunk and the
 * source's k-way merge does all the real work.
 */
auto RunSort(const std::vector<TestRow> &rows, OrderByType dir, idx_t chunk_size, idx_t budget = MAX_MEMORY)
    -> std::vector<TestRow> {
  OperatorHarness h;
  h.client.mem_.SetBudget(budget);
  auto schema = TwoColSchema();
  auto scan = std::make_unique<RowScan>(schema, rows, chunk_size);
  auto sort = std::make_unique<PhysicalExternalMergeSort>(schema, OrderOn(schema, 0, dir), std::move(scan));
  RowCollector collector(std::move(sort));
  return h.Run(collector);
}

auto Row(int32_t k, const std::string &s) -> TestRow { return {Value(k), Value(s)}; }
auto NullKeyRow(const std::string &s) -> TestRow { return {Value::Null(kInt), Value(s)}; }

}  // namespace

TEST(PhysicalExternalMergeSortTest, OrdersAscendingAndDescending) {
  const std::vector<TestRow> rows{Row(3, "c"), Row(1, "a"), Row(2, "b")};
  EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::ASC, 16)), (std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::DESC, 16)), (std::vector<int32_t>{3, 2, 1}));
}

// The engine's fixed convention: NULLs last in ASC, first in DESC — encoded in the run keys, so it
// must survive the spill-and-merge round trip too.
TEST(PhysicalExternalMergeSortTest, NullsLastAscendingFirstDescending) {
  const std::vector<TestRow> rows{Row(2, "b"), NullKeyRow("n"), Row(1, "a")};

  auto asc = RunSort(rows, OrderByType::ASC, 16);
  ASSERT_EQ(asc.size(), 3U);
  EXPECT_FALSE(asc[0][0].IsNull());
  EXPECT_FALSE(asc[1][0].IsNull());
  EXPECT_TRUE(asc[2][0].IsNull()) << "ASC puts NULLs last";

  auto desc = RunSort(rows, OrderByType::DESC, 16);
  ASSERT_EQ(desc.size(), 3U);
  EXPECT_TRUE(desc[0][0].IsNull()) << "DESC puts NULLs first";
}

// Budget 0 flushes a run per input chunk: 50 rows in chunks of 7 is 8 runs, so the answer can only
// come out right if the min-heap merge across run cursors is right.
TEST(PhysicalExternalMergeSortTest, MergesManyRunsInOrder) {
  std::vector<TestRow> rows;
  rows.reserve(50);
  for (int i = 0; i < 50; i++) {
    rows.push_back(Row((i * 17) % 50, "v" + std::to_string(i)));  // a fixed shuffle, no RNG needed
  }
  std::vector<int32_t> expect(50);
  for (int i = 0; i < 50; i++) {
    expect[i] = i;
  }
  EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::ASC, 7, /*budget=*/0)), expect);
}

// String payloads live in a heap owned by the vector they were built in; spilling a run and
// gathering it back must re-home them, not leave views into freed memory. Long values defeat the
// 11-byte inline optimization so the off-row path is actually taken (ASan is the real assertion).
TEST(PhysicalExternalMergeSortTest, SpilledStringPayloadsSurviveTheRoundTrip) {
  const std::string long_a(200, 'a');
  const std::string long_b(200, 'b');
  const std::vector<TestRow> rows{Row(2, long_b), Row(1, long_a)};
  auto out = RunSort(rows, OrderByType::ASC, 1, /*budget=*/0);
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0][1].GetString(), long_a);
  EXPECT_EQ(out[1][1].GetString(), long_b);
}

TEST(PhysicalExternalMergeSortTest, ResultIsIndependentOfInputChunkingAndBudget) {
  std::vector<TestRow> rows;
  rows.reserve(50);
  for (int i = 49; i >= 0; i--) {
    rows.push_back(Row(i, "v" + std::to_string(i)));
  }
  const auto one_chunk = KeyColumn(RunSort(rows, OrderByType::ASC, 64));
  for (const idx_t chunk : {idx_t{1}, idx_t{7}, idx_t{50}}) {
    EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::ASC, chunk)), one_chunk) << "chunk size " << chunk;
    EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::ASC, chunk, /*budget=*/0)), one_chunk)
        << "chunk size " << chunk << ", budget 0";
  }
}

TEST(PhysicalExternalMergeSortTest, EmptyInputProducesNoRows) {
  EXPECT_TRUE(RunSort({}, OrderByType::ASC, 16).empty());
  EXPECT_TRUE(RunSort({}, OrderByType::ASC, 16, /*budget=*/0).empty());
}

TEST(PhysicalExternalMergeSortTest, KeepsDuplicateKeysAcrossRuns) {
  const std::vector<TestRow> rows{Row(1, "a"), Row(1, "b"), Row(1, "c")};
  auto out = RunSort(rows, OrderByType::ASC, 1, /*budget=*/0);  // each duplicate lands in its own run
  EXPECT_EQ(out.size(), 3U) << "a sort must not deduplicate";
  EXPECT_EQ(KeyColumn(out), (std::vector<int32_t>{1, 1, 1}));
}

}  // namespace bumblebee
