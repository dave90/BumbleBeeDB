//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// serializable_test.cpp
//
// Identification: test/unit/concurrency/serializable_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/mvcc/mvcc.h"
#include "storage/row/row_layout.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

namespace {

auto TypesOf(const Schema &schema) -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  for (const auto &c : schema.GetColumns()) {
    types.push_back(c.GetType());
  }
  return types;
}

/** Read column 0 (an INTEGER) straight out of a row's RowLayout bytes. */
auto ReadIntColumn(const RowLayout &layout, const_data_ptr_t row) -> int32_t {
  return *reinterpret_cast<const int32_t *>(row + layout.GetOffsets()[0]);
}

/** A one-column (INTEGER v) MVCC table with a transaction manager, for serializable tests. */
struct SerFixture {
  MemoryDiskManager dm{256};
  BufferPoolManager bpm{16, &dm};
  Catalog catalog{&bpm};
  TransactionManager tm{&catalog};
  std::shared_ptr<TableInfo> table;

  SerFixture() {
    std::vector<Column> cols{Column("v", LogicalType(LogicalTypeId::INTEGER))};
    table = catalog.CreateTable("t", Schema(cols));
  }

  auto Heap() -> TableHeap & { return static_cast<TableHeap &>(*table->storage_); }
  auto Oid() -> table_oid_t { return table->oid_; }

  auto OneRow(int32_t v) -> DataChunk {
    DataChunk c;
    c.Initialize(TypesOf(table->schema_));
    c.SetValue(0, 0, Value(v));
    c.SetCardinality(1);
    return c;
  }

  auto Insert(Transaction *txn, int32_t v) -> RID {
    auto chunk = OneRow(v);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    MvccInsert(&tm, txn, Oid(), Heap(), chunk, rids);
    return RID(FlatVector::GetData<int64_t>(rids)[0]);
  }

  void Update(Transaction *txn, RID rid, int32_t v) {
    auto chunk = OneRow(v);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    FlatVector::GetData<int64_t>(rids)[0] = rid.Get();
    MvccUpdate(&tm, txn, Oid(), Heap(), rids, chunk);
  }

  /** Record that `txn` read every row whose v equals `wanted` (the predicate it evaluated). */
  void RecordReadWhereEquals(Transaction *txn, int32_t wanted) {
    txn->AppendScanPredicate(Oid(), [wanted](const RowLayout &layout, const_data_ptr_t row) {
      return ReadIntColumn(layout, row) == wanted;
    });
  }
};

}  // namespace

// Write-skew: two serializable txns each read the rows matching a predicate and flip one of them.
// Committing both would break the invariant, so the second to commit must be rejected.
TEST(SerializableTest, WriteSkewRejectsSecondCommitter) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1);
  auto b = f.Insert(seed, 1);
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *t2 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  auto *t3 = f.tm.Begin(IsolationLevel::SERIALIZABLE);

  // Both read the set {v == 1} (they each saw a=1 and b=1), then each zeroes one of the rows.
  f.RecordReadWhereEquals(t2, 1);
  f.RecordReadWhereEquals(t3, 1);
  f.Update(t2, a, 0);  // t2: set a = 0 where a matched v==1
  f.Update(t3, b, 0);  // t3: set b = 0 where b matched v==1

  ASSERT_TRUE(f.tm.Commit(t2)) << "first committer succeeds";
  EXPECT_FALSE(f.tm.Commit(t3)) << "second committer read a row t2 modified — must abort";
  EXPECT_EQ(t3->GetTransactionState(), TransactionState::ABORTED);

  // The invariant survived: exactly one row was zeroed (a=0, b=1).
  auto *reader = f.tm.Begin();
  auto scan = f.Heap().MakeMvccScan(&f.tm, reader, f.Oid());
  DataChunk out;
  out.Initialize(TypesOf(f.table->schema_));
  int ones = 0;
  int zeroes = 0;
  while (scan->Next(out)) {
    for (idx_t i = 0; i < out.GetSize(); i++) {
      (out.GetValue(0, i).GetAs<int32_t>() == 1 ? ones : zeroes)++;
    }
  }
  EXPECT_EQ(ones, 1);
  EXPECT_EQ(zeroes, 1) << "t3's zeroing of b was rolled back, so only a is 0";
}

// A serializable txn that only wrote rows nobody else touched commits fine.
TEST(SerializableTest, NonConflictingSerializableCommits) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1);
  auto b = f.Insert(seed, 2);
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *t2 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  auto *t3 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  f.RecordReadWhereEquals(t2, 1);  // t2 read {v==1} = {a}
  f.RecordReadWhereEquals(t3, 2);  // t3 read {v==2} = {b}
  f.Update(t2, a, 10);             // t2 modifies a (which t3 did not read)
  f.Update(t3, b, 20);            // t3 modifies b (which t2 did not read)

  EXPECT_TRUE(f.tm.Commit(t2));
  EXPECT_TRUE(f.tm.Commit(t3)) << "disjoint read/write sets are serializable";
}

// The scan itself registers the read predicate: a serializable txn that scans the table and then
// writes must abort if a concurrent txn committed a change to that table — no manual predicate needed.
TEST(SerializableTest, ScanAutoRegistersReadPredicate) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1);
  f.Insert(seed, 2);
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *t2 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  // t2 scans the whole table (auto-registering a whole-table read predicate), then writes a row.
  {
    auto scan = f.Heap().MakeMvccScan(&f.tm, t2, f.Oid());
    DataChunk out;
    out.Initialize(TypesOf(f.table->schema_));
    while (scan->Next(out)) {
    }
  }
  f.Update(t2, a, 100);

  // A concurrent txn commits a change to the *other* row of the same table after t2's snapshot.
  auto other = RID(a.GetPageId(), a.GetSlotNum() == 0 ? 1 : 0);
  auto *t3 = f.tm.Begin();
  f.Update(t3, other, 200);
  ASSERT_TRUE(f.tm.Commit(t3));

  // t2's scan read the whole table (including the row t3 changed), so it must not serialize.
  EXPECT_FALSE(f.tm.Commit(t2)) << "a scanned-then-written serializable txn conflicts with a concurrent commit";
  EXPECT_EQ(t2->GetTransactionState(), TransactionState::ABORTED);
}

// A read-only serializable txn always commits (it observed one consistent snapshot).
TEST(SerializableTest, ReadOnlySerializableAlwaysCommits) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1);
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *ro = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  f.RecordReadWhereEquals(ro, 1);

  // A concurrent writer commits a change to the row the read-only txn read.
  auto *w = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  f.Update(w, a, 5);
  ASSERT_TRUE(f.tm.Commit(w));

  EXPECT_TRUE(f.tm.Commit(ro)) << "a read-only txn never conflicts";
}

// Two threads run the conflicting write-skew txns concurrently; exactly one commits.
TEST(SerializableTest, ConcurrentWriteSkewExactlyOneCommits) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1);
  auto b = f.Insert(seed, 1);
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *t2 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  auto *t3 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  f.RecordReadWhereEquals(t2, 1);
  f.RecordReadWhereEquals(t3, 1);
  f.Update(t2, a, 0);
  f.Update(t3, b, 0);

  std::atomic<int> commits{0};
  std::vector<std::thread> threads;
  for (auto *t : {t2, t3}) {
    threads.emplace_back([&f, &commits, t]() {
      if (f.tm.Commit(t)) {
        commits.fetch_add(1);
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  EXPECT_EQ(commits.load(), 1) << "commit-time validation admits exactly one";
}

// A predicate passed to MakeMvccScan filters the scan's output to the rows it matches.
TEST(SerializableTest, ScanPredicateFiltersRows) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  f.Insert(seed, 1);
  f.Insert(seed, 2);
  f.Insert(seed, 1);
  f.Insert(seed, 3);
  ASSERT_TRUE(f.tm.Commit(seed));

  // Scan only the rows where v == 1 (a snapshot reader, so no serializable bookkeeping).
  auto *reader = f.tm.Begin();
  auto scan = f.Heap().MakeMvccScan(&f.tm, reader, f.Oid(),
                                    [](const RowLayout &layout, const_data_ptr_t row) { return ReadIntColumn(layout, row) == 1; });
  DataChunk out;
  out.Initialize(TypesOf(f.table->schema_));
  int rows = 0;
  while (scan->Next(out)) {
    for (idx_t i = 0; i < out.GetSize(); i++) {
      EXPECT_EQ(out.GetValue(0, i).GetAs<int32_t>(), 1) << "only matching rows are returned";
      rows++;
    }
  }
  EXPECT_EQ(rows, 2) << "exactly the two v==1 rows";
}

// The predicate a filtered scan registers is precise: a concurrent commit to a row OUTSIDE the
// predicate does not conflict, so the serializable txn still commits (no false positive).
TEST(SerializableTest, FilteredScanPredicateIsPrecise) {
  SerFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1);  // matches v==1
  auto b = f.Insert(seed, 2);  // does not match
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *t2 = f.tm.Begin(IsolationLevel::SERIALIZABLE);
  // t2 scans only {v==1} — the filtered scan auto-registers exactly that predicate as its read set.
  {
    auto scan = f.Heap().MakeMvccScan(&f.tm, t2, f.Oid(),
                                      [](const RowLayout &layout, const_data_ptr_t row) { return ReadIntColumn(layout, row) == 1; });
    DataChunk out;
    out.Initialize(TypesOf(f.table->schema_));
    while (scan->Next(out)) {
    }
  }
  f.Update(t2, a, 100);  // t2 writes a matching row

  // A concurrent txn commits a change to b (v==2), which is OUTSIDE t2's read predicate.
  auto *t3 = f.tm.Begin();
  f.Update(t3, b, 200);
  ASSERT_TRUE(f.tm.Commit(t3));

  EXPECT_TRUE(f.tm.Commit(t2)) << "the concurrent write missed t2's read predicate — no phantom, so it serializes";
}

}  // namespace bumblebee
