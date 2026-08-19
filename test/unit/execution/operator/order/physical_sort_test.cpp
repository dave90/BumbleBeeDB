//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_sort_test.cpp
//
// Identification: test/unit/execution/operator/order/physical_sort_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "execution/operator/order/physical_sort.h"
#include "execution/operator/order/physical_top_n.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);
const LogicalType kStr(LogicalTypeId::STRING);

auto TwoColSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"s", kStr}}); }

/** @brief One ORDER BY key over column `col` of `schema`. */
auto OrderOn(const SchemaRef &schema, uint32_t col, OrderByType dir) -> std::vector<OrderBy> {
  std::vector<OrderBy> obs;
  obs.emplace_back(dir, OrderByNullType::DEFAULT, ColRef(schema, col));
  return obs;
}

/** @brief Column 0 of every output row, in order. */
auto KeyColumn(const std::vector<TestRow> &rows) -> std::vector<int32_t> {
  std::vector<int32_t> out;
  out.reserve(rows.size());
  for (const auto &r : rows) {
    out.push_back(r[0].IsNull() ? INT32_MIN : r[0].GetAs<int32_t>());
  }
  return out;
}

/** @brief Run a sort over `rows`, emitting the input `chunk_size` rows at a time. */
auto RunSort(const std::vector<TestRow> &rows, OrderByType dir, idx_t chunk_size) -> std::vector<TestRow> {
  OperatorHarness h;
  auto schema = TwoColSchema();
  auto scan = std::make_unique<RowScan>(schema, rows, chunk_size);
  auto sort = std::make_unique<PhysicalSort>(schema, OrderOn(schema, 0, dir), std::move(scan));
  RowCollector collector(std::move(sort));
  return h.Run(collector);
}

auto Row(int32_t k, const std::string &s) -> TestRow { return {Value(k), Value(s)}; }
auto NullKeyRow(const std::string &s) -> TestRow { return {Value::Null(kInt), Value(s)}; }

}  // namespace

TEST(PhysicalSortTest, OrdersAscendingAndDescending) {
  const std::vector<TestRow> rows{Row(3, "c"), Row(1, "a"), Row(2, "b")};
  EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::ASC, 16)), (std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::DESC, 16)), (std::vector<int32_t>{3, 2, 1}));
}

// The engine's fixed convention (CLAUDE.md): NULLs last in ASC, first in DESC. `NULLS FIRST/LAST`
// is parsed but ignored, so this is the only ordering a query can get.
TEST(PhysicalSortTest, NullsLastAscendingFirstDescending) {
  const std::vector<TestRow> rows{Row(2, "b"), NullKeyRow("n"), Row(1, "a")};

  auto asc = RunSort(rows, OrderByType::ASC, 16);
  ASSERT_EQ(asc.size(), 3u);
  EXPECT_FALSE(asc[0][0].IsNull());
  EXPECT_FALSE(asc[1][0].IsNull());
  EXPECT_TRUE(asc[2][0].IsNull()) << "ASC puts NULLs last";

  auto desc = RunSort(rows, OrderByType::DESC, 16);
  ASSERT_EQ(desc.size(), 3u);
  EXPECT_TRUE(desc[0][0].IsNull()) << "DESC puts NULLs first";
}

// A sort is a pipeline breaker fed chunk by chunk; splitting the same input across several chunks
// must not change the result. This is the multi-chunk path the small-vector build exists for,
// reachable here on the normal build.
TEST(PhysicalSortTest, ResultIsIndependentOfInputChunking) {
  std::vector<TestRow> rows;
  rows.reserve(50);
  for (int i = 49; i >= 0; i--) {
    rows.push_back(Row(i, "v" + std::to_string(i)));
  }
  const auto one_chunk = KeyColumn(RunSort(rows, OrderByType::ASC, 64));
  for (const idx_t chunk : {idx_t{1}, idx_t{7}, idx_t{50}}) {
    EXPECT_EQ(KeyColumn(RunSort(rows, OrderByType::ASC, chunk)), one_chunk) << "chunk size " << chunk;
  }
}

TEST(PhysicalSortTest, EmptyInputProducesNoRows) { EXPECT_TRUE(RunSort({}, OrderByType::ASC, 16).empty()); }

TEST(PhysicalSortTest, KeepsDuplicateKeys) {
  const std::vector<TestRow> rows{Row(1, "a"), Row(1, "b"), Row(1, "c")};
  auto out = RunSort(rows, OrderByType::ASC, 2);
  EXPECT_EQ(out.size(), 3u) << "a sort must not deduplicate";
  EXPECT_EQ(KeyColumn(out), (std::vector<int32_t>{1, 1, 1}));
}

// ---------------------------------------------------------------------------------------------
// TopN. `ORDER BY x LIMIT n` always lowers to TopN (OptimizeSortLimitAsTopN), so these are the
// paths a limited query actually takes.
// ---------------------------------------------------------------------------------------------

namespace {

auto RunTopN(const std::vector<TestRow> &rows, OrderByType dir, std::size_t n, idx_t chunk_size)
    -> std::vector<TestRow> {
  OperatorHarness h;
  auto schema = TwoColSchema();
  auto scan = std::make_unique<RowScan>(schema, rows, chunk_size);
  auto top_n = std::make_unique<PhysicalTopN>(schema, OrderOn(schema, 0, dir), n, std::move(scan));
  RowCollector collector(std::move(top_n));
  return h.Run(collector);
}

}  // namespace

TEST(PhysicalTopNTest, KeepsSmallestNAscending) {
  std::vector<TestRow> rows;
  for (int i = 20; i >= 1; i--) {
    rows.push_back(Row(i, "v"));
  }
  EXPECT_EQ(KeyColumn(RunTopN(rows, OrderByType::ASC, 3, 4)), (std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ(KeyColumn(RunTopN(rows, OrderByType::DESC, 3, 4)), (std::vector<int32_t>{20, 19, 18}));
}

// n larger than the input must yield the whole input, not n rows padded somehow.
TEST(PhysicalTopNTest, LimitLargerThanInputReturnsEverything) {
  const std::vector<TestRow> rows{Row(2, "b"), Row(1, "a")};
  EXPECT_EQ(KeyColumn(RunTopN(rows, OrderByType::ASC, 100, 1)), (std::vector<int32_t>{1, 2}));
}

TEST(PhysicalTopNTest, ZeroLimitProducesNoRows) {
  const std::vector<TestRow> rows{Row(2, "b"), Row(1, "a")};
  EXPECT_TRUE(RunTopN(rows, OrderByType::ASC, 0, 1).empty());
}

TEST(PhysicalTopNTest, EmptyInputProducesNoRows) { EXPECT_TRUE(RunTopN({}, OrderByType::ASC, 5, 1).empty()); }

// The heap is bounded, so which rows survive depends on the order they arrive in. Chunking the
// same input differently must still select the same n.
TEST(PhysicalTopNTest, SelectionIsIndependentOfInputChunking) {
  std::vector<TestRow> rows;
  for (int i = 0; i < 40; i++) {
    rows.push_back(Row((i * 17) % 40, "v"));  // a fixed shuffle, no RNG needed
  }
  const auto baseline = KeyColumn(RunTopN(rows, OrderByType::ASC, 5, 64));
  for (const idx_t chunk : {idx_t{1}, idx_t{3}, idx_t{40}}) {
    EXPECT_EQ(KeyColumn(RunTopN(rows, OrderByType::ASC, 5, chunk)), baseline) << "chunk size " << chunk;
  }
}

// NULL keys must not crowd out real values at the front of an ASC top-n.
TEST(PhysicalTopNTest, NullKeysSortLastAscending) {
  const std::vector<TestRow> rows{NullKeyRow("n1"), Row(5, "a"), NullKeyRow("n2"), Row(3, "b")};
  auto out = RunTopN(rows, OrderByType::ASC, 2, 2);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(KeyColumn(out), (std::vector<int32_t>{3, 5})) << "the two non-NULL keys win";
}

}  // namespace bumblebee
