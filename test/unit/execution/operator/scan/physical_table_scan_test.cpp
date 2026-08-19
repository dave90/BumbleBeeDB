//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_table_scan_test.cpp
//
// Identification: test/unit/execution/operator/scan/physical_table_scan_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "execution/operator/scan/physical_table_scan.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"
#include "storage/table/table_heap.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);
const LogicalType kBigint(LogicalTypeId::BIGINT);

auto TableSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"v", kInt}}); }

/** @brief Create table `name` and append `n` rows of (i, i * 10) straight into its heap.
 *
 * Direct heap appends carry no MVCC version info, so the rows are visible to every snapshot —
 * the same seeding idiom as the end-to-end tests. Returns the table oid. */
auto SeedTable(OperatorHarness &h, const std::string &name, int n) -> table_oid_t {
  auto info = h.catalog->CreateTable(name, *TableSchema());
  EXPECT_NE(info, NULL_TABLE_INFO);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  EXPECT_NE(heap, nullptr);
  int written = 0;
  while (written < n) {
    const idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(std::vector<LogicalType>{kInt, kInt});
    for (idx_t i = 0; i < batch; i++) {
      const int idx = written + static_cast<int>(i);
      chunk.SetValue(0, i, Value(idx));
      chunk.SetValue(1, i, Value(idx * 10));
    }
    chunk.SetCardinality(batch);
    Vector rids{kBigint};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
  return info->oid_;
}

/** @brief Run `scan` in its own committed transaction. */
auto RunScan(OperatorHarness &h, std::unique_ptr<PhysicalOperator> scan) -> std::vector<TestRow> {
  auto *txn = h.txn_mgr.Begin();
  h.client.txn_ = txn;
  RowCollector collector(std::move(scan));
  auto rows = h.Run(collector);
  EXPECT_TRUE(h.txn_mgr.Commit(txn));
  h.client.txn_ = nullptr;
  SortRows(rows);
  return rows;
}

}  // namespace

TEST(PhysicalTableScanTest, ReturnsEveryRow) {
  OperatorHarness h;
  auto oid = SeedTable(h, "t", 5);
  auto rows = RunScan(h, std::make_unique<PhysicalTableScan>(TableSchema(), oid, "t", 5));
  ASSERT_EQ(rows.size(), 5U);
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 0);
  EXPECT_EQ(rows[4][1].GetAs<int32_t>(), 40);
}

TEST(PhysicalTableScanTest, EmptyTableProducesNoRows) {
  OperatorHarness h;
  auto oid = SeedTable(h, "t", 0);
  EXPECT_TRUE(RunScan(h, std::make_unique<PhysicalTableScan>(TableSchema(), oid, "t", 0)).empty());
}

// Column pruning's contract (CLAUDE.md): schemas stay full-width, pruned slots surface as
// constant-NULL vectors nothing reads — no renumbering. A scan projecting only column 1 must
// still emit two columns, with column 0 all-NULL.
TEST(PhysicalTableScanTest, ProjectionLeavesPrunedColumnsNull) {
  OperatorHarness h;
  auto oid = SeedTable(h, "t", 4);
  auto rows = RunScan(
      h, std::make_unique<PhysicalTableScan>(TableSchema(), oid, "t", 4, ScanPredicate{}, std::vector<idx_t>{1}));
  ASSERT_EQ(rows.size(), 4U);
  std::multiset<int32_t> vs;
  for (const auto &r : rows) {
    ASSERT_EQ(r.size(), 2U) << "schema stays full-width";
    EXPECT_TRUE(r[0].IsNull()) << "the pruned column reads as NULL";
    ASSERT_FALSE(r[1].IsNull());
    vs.insert(r[1].GetAs<int32_t>());
  }
  EXPECT_EQ(vs, (std::multiset<int32_t>{0, 10, 20, 30}));
}

// The DML child mode: a trailing BIGINT __rid, non-NULL and unique per row — what Update/Delete
// address their writes with.
TEST(PhysicalTableScanTest, EmitRidsAppendsAUniqueRowId) {
  OperatorHarness h;
  auto oid = SeedTable(h, "t", 6);
  auto rid_schema = MakeSchemaOf({{"k", kInt}, {"v", kInt}, {"__rid", kBigint}});
  auto rows = RunScan(h, std::make_unique<PhysicalTableScan>(rid_schema, oid, "t", 6, ScanPredicate{},
                                                             std::vector<idx_t>{}, /*emit_rids=*/true));
  ASSERT_EQ(rows.size(), 6U);
  std::set<int64_t> rids;
  for (const auto &r : rows) {
    ASSERT_EQ(r.size(), 3U);
    ASSERT_FALSE(r[2].IsNull());
    rids.insert(r[2].GetAs<int64_t>());
  }
  EXPECT_EQ(rids.size(), 6U) << "every row gets a distinct RID";
}

// Enough rows to span many heap pages, so the morsel-claiming path (shared ParallelScanState,
// per-task cursors) does real work rather than serving one morsel.
TEST(PhysicalTableScanTest, ScansAcrossManyPages) {
  OperatorHarness h;
  const int n = 5000;
  auto oid = SeedTable(h, "t", n);
  auto rows = RunScan(h, std::make_unique<PhysicalTableScan>(TableSchema(), oid, "t", n));
  ASSERT_EQ(rows.size(), static_cast<size_t>(n));
  std::set<int32_t> ks;
  for (const auto &r : rows) {
    ks.insert(r[0].GetAs<int32_t>());
  }
  EXPECT_EQ(ks.size(), static_cast<size_t>(n)) << "no row duplicated or dropped across morsels";
}

}  // namespace bumblebee
