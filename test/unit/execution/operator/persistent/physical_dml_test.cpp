//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_dml_test.cpp
//
// Identification: test/unit/execution/operator/persistent/physical_dml_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "common/exception.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/operator/persistent/physical_delete.h"
#include "execution/operator/persistent/physical_insert.h"
#include "execution/operator/persistent/physical_update.h"
#include "execution/operator/scan/physical_table_scan.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);
const LogicalType kBigint(LogicalTypeId::BIGINT);

// The write operators (insert/update/delete) driven directly, against a real bpm-backed TableHeap
// and real transactions — the missing middle between the MVCC storage-layer tests and the e2e SQL
// corpus. The table is created straight through the catalog with neither auto `_id` nor a PK
// index, so these tests exercise the plain MvccInsert/MvccUpdate/MvccDelete paths; the PK/index
// interplay is covered by the e2e corpus.
auto TableSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"v", kInt}}); }
/** Every DML operator's source role emits one row: the affected-row count, a single INTEGER. */
auto CountSchema() -> SchemaRef { return MakeSchemaOf({{"rows", kInt}}); }
/** What a DML child scan emits: the table columns plus the trailing `__rid` BIGINT. */
auto RidSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"v", kInt}, {"__rid", kBigint}}); }

auto Row(int32_t k, int32_t v) -> TestRow { return {Value(k), Value(v)}; }

auto MakeTable(OperatorHarness &h, const std::string &name) -> table_oid_t {
  auto info = h.catalog->CreateTable(name, *TableSchema());
  EXPECT_NE(info, NULL_TABLE_INFO);
  return info->oid_;
}

/** @brief Run `root` inside its own committed transaction and return the collected rows. */
auto RunInTxn(OperatorHarness &h, PhysicalOperator &root) -> std::vector<TestRow> {
  auto *txn = h.txn_mgr.Begin();
  h.client.txn_ = txn;
  auto rows = h.Run(root);
  EXPECT_TRUE(h.txn_mgr.Commit(txn));
  h.client.txn_ = nullptr;
  return rows;
}

/** @brief Insert `rows` into `oid` in one committed transaction; returns the reported count. */
auto Insert(OperatorHarness &h, table_oid_t oid, const std::vector<TestRow> &rows, idx_t chunk_size = 16) -> int32_t {
  auto scan = std::make_unique<RowScan>(TableSchema(), rows, chunk_size);
  auto insert = std::make_unique<PhysicalInsert>(CountSchema(), oid, std::move(scan));
  RowCollector collector(std::move(insert));
  auto out = RunInTxn(h, collector);
  EXPECT_EQ(out.size(), 1U) << "a DML statement reports exactly one count row";
  return out[0][0].GetAs<int32_t>();
}

/** @brief The rid-emitting table scan every UPDATE/DELETE sits on. */
auto MakeRidScan(table_oid_t oid) -> std::unique_ptr<PhysicalTableScan> {
  return std::make_unique<PhysicalTableScan>(RidSchema(), oid, "t", /*estimated_cardinality=*/1, ScanPredicate{},
                                             std::vector<idx_t>{}, /*emit_rids=*/true);
}

/** @brief Scan the table back in a fresh committed transaction, sorted for stable comparison. */
auto ScanAll(OperatorHarness &h, table_oid_t oid) -> std::vector<TestRow> {
  auto scan = std::make_unique<PhysicalTableScan>(TableSchema(), oid, "t", /*estimated_cardinality=*/1);
  RowCollector collector(std::move(scan));
  auto rows = RunInTxn(h, collector);
  SortRows(rows);
  return rows;
}

}  // namespace

TEST(PhysicalInsertTest, InsertsRowsAndReportsCount) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  EXPECT_EQ(Insert(h, oid, {Row(1, 10), Row(2, 20), Row(3, 30)}), 3);
  auto rows = ScanAll(h, oid);
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 1);
  EXPECT_EQ(rows[0][1].GetAs<int32_t>(), 10);
  EXPECT_EQ(rows[2][1].GetAs<int32_t>(), 30);
}

TEST(PhysicalInsertTest, CountIsIndependentOfInputChunking) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  std::vector<TestRow> rows;
  rows.reserve(100);
  for (int i = 0; i < 100; i++) {
    rows.push_back(Row(i, i * 2));
  }
  EXPECT_EQ(Insert(h, oid, rows, /*chunk_size=*/7), 100);
  EXPECT_EQ(ScanAll(h, oid).size(), 100U);
}

TEST(PhysicalInsertTest, EmptyInputInsertsNothing) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  EXPECT_EQ(Insert(h, oid, {}), 0);
  EXPECT_TRUE(ScanAll(h, oid).empty());
}

TEST(PhysicalDeleteTest, DeletesEveryScannedRow) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  Insert(h, oid, {Row(1, 10), Row(2, 20), Row(3, 30)});

  auto del = std::make_unique<PhysicalDelete>(CountSchema(), oid, MakeRidScan(oid), /*rid_column=*/2);
  RowCollector collector(std::move(del));
  auto out = RunInTxn(h, collector);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0][0].GetAs<int32_t>(), 3);
  EXPECT_TRUE(ScanAll(h, oid).empty());
}

// MVCC: a snapshot taken before the delete commits must keep seeing the rows — the delete
// tombstones a new version, it does not destroy the one the older snapshot reads.
TEST(PhysicalDeleteTest, SnapshotFromBeforeTheDeleteStillSeesTheRows) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  Insert(h, oid, {Row(1, 10), Row(2, 20)});

  auto *reader = h.txn_mgr.Begin();  // snapshot: both rows live

  {
    auto del = std::make_unique<PhysicalDelete>(CountSchema(), oid, MakeRidScan(oid), /*rid_column=*/2);
    RowCollector collector(std::move(del));
    RunInTxn(h, collector);
  }

  h.client.txn_ = reader;
  auto scan = std::make_unique<PhysicalTableScan>(TableSchema(), oid, "t", 1);
  RowCollector collector(std::move(scan));
  auto rows = h.Run(collector);
  EXPECT_EQ(rows.size(), 2U) << "the pre-delete snapshot reads the pre-delete versions";
  EXPECT_TRUE(h.txn_mgr.Commit(reader));
  h.client.txn_ = nullptr;

  EXPECT_TRUE(ScanAll(h, oid).empty()) << "a fresh snapshot sees the delete";
}

TEST(PhysicalUpdateTest, RewritesTargetColumnAndKeepsTheOther) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  Insert(h, oid, {Row(1, 10), Row(2, 20), Row(3, 30)});

  // One target expression per table column, evaluated over the child's (k, v, __rid) output:
  // k stays itself, v becomes the constant 99.
  auto rid_schema = RidSchema();
  std::vector<AbstractExpressionRef> targets{ColRef(rid_schema, 0),
                                             std::make_shared<ConstantValueExpression>(Value(99))};
  auto update =
      std::make_unique<PhysicalUpdate>(CountSchema(), oid, MakeRidScan(oid), /*rid_column=*/2, std::move(targets));
  RowCollector collector(std::move(update));
  auto out = RunInTxn(h, collector);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0][0].GetAs<int32_t>(), 3);

  auto rows = ScanAll(h, oid);
  ASSERT_EQ(rows.size(), 3U);
  for (const auto &r : rows) {
    EXPECT_EQ(r[1].GetAs<int32_t>(), 99);
  }
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 1) << "the untouched column keeps its value";
  EXPECT_EQ(rows[2][0].GetAs<int32_t>(), 3);
}

TEST(PhysicalUpdateTest, TargetCanReadTheOldRow) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  Insert(h, oid, {Row(1, 10), Row(2, 20)});

  // v := k — per-row recompute from the pre-image, not a constant fill.
  auto rid_schema = RidSchema();
  std::vector<AbstractExpressionRef> targets{ColRef(rid_schema, 0), ColRef(rid_schema, 0)};
  auto update =
      std::make_unique<PhysicalUpdate>(CountSchema(), oid, MakeRidScan(oid), /*rid_column=*/2, std::move(targets));
  RowCollector collector(std::move(update));
  RunInTxn(h, collector);

  auto rows = ScanAll(h, oid);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0][1].GetAs<int32_t>(), 1);
  EXPECT_EQ(rows[1][1].GetAs<int32_t>(), 2);
}

// First-committer-wins: a transaction that modifies a row already modified by a committed-later
// sibling must fail, not silently double-apply.
TEST(PhysicalDeleteTest, WriteWriteConflictIsRejected) {
  OperatorHarness h;
  auto oid = MakeTable(h, "t");
  Insert(h, oid, {Row(1, 10)});

  auto *loser = h.txn_mgr.Begin();  // starts before the winner commits, writes after

  {
    auto del = std::make_unique<PhysicalDelete>(CountSchema(), oid, MakeRidScan(oid), /*rid_column=*/2);
    RowCollector collector(std::move(del));
    RunInTxn(h, collector);  // the winner deletes the row and commits
  }

  h.client.txn_ = loser;
  auto del = std::make_unique<PhysicalDelete>(CountSchema(), oid, MakeRidScan(oid), /*rid_column=*/2);
  RowCollector collector(std::move(del));
  bool conflicted = false;
  try {
    h.Run(collector);
    conflicted = !h.txn_mgr.Commit(loser);
  } catch (const Exception &) {
    conflicted = true;  // surfaced during execution: abort, first-committer-wins held
    h.txn_mgr.Abort(loser);
  }
  h.client.txn_ = nullptr;
  EXPECT_TRUE(conflicted) << "the second writer must not win";
}

}  // namespace bumblebee
