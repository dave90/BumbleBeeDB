//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// mvcc_write_test.cpp
//
// Identification: test/unit/concurrency/mvcc_write_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/mvcc/mvcc.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** A minimal MVCC test rig: a buffer pool, a one-table catalog, and a transaction manager. */
struct MvccFixture {
  MemoryDiskManager dm{256};
  BufferPoolManager bpm{16, &dm};
  Catalog catalog{&bpm};
  TransactionManager tm{&catalog};
  std::shared_ptr<TableInfo> table;

  MvccFixture() {
    std::vector<Column> cols{
        Column("id", LogicalType(LogicalTypeId::INTEGER)),
        Column("name", LogicalType(LogicalTypeId::STRING), VARCHAR_DEFAULT_LENGTH),
    };
    table = catalog.CreateTable("t", Schema(cols));
  }

  auto Heap() -> TableHeap & { return static_cast<TableHeap &>(*table->storage_); }
  auto Oid() -> table_oid_t { return table->oid_; }

  auto OneRow(int32_t id, const std::string &name) -> DataChunk {
    DataChunk c;
    c.Initialize(table->schema_.GetTypes());
    c.SetValue(0, 0, Value(id));
    c.SetValue(1, 0, Value(name));
    c.SetCardinality(1);
    return c;
  }

  /** Insert one row under `txn`, returning its RID. */
  auto Insert(Transaction *txn, int32_t id, const std::string &name) -> RID {
    auto chunk = OneRow(id, name);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    MvccInsert(&tm, txn, Oid(), Heap(), chunk, rids);
    return RID(FlatVector::GetData<int64_t>(rids)[0]);
  }

  /** Build a chunk of many (id, name) rows. */
  auto ManyRows(const std::vector<std::pair<int32_t, std::string>> &rows) -> DataChunk {
    DataChunk c;
    c.Initialize(table->schema_.GetTypes());
    for (idx_t i = 0; i < rows.size(); i++) {
      c.SetValue(0, i, Value(rows[i].first));
      c.SetValue(1, i, Value(rows[i].second));
    }
    c.SetCardinality(rows.size());
    return c;
  }

  /** Insert a batch of rows under `txn` in a single MvccInsert call, returning their RIDs in order. */
  auto InsertBatch(Transaction *txn, const std::vector<std::pair<int32_t, std::string>> &rows) -> std::vector<RID> {
    auto chunk = ManyRows(rows);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    MvccInsert(&tm, txn, Oid(), Heap(), chunk, rids);
    auto *d = FlatVector::GetData<int64_t>(rids);
    std::vector<RID> out;
    out.reserve(rows.size());
    for (idx_t i = 0; i < rows.size(); i++) {
      out.emplace_back(d[i]);
    }
    return out;
  }

  /** Update a batch of rows under `txn` in a single MvccUpdate call. */
  void UpdateBatch(Transaction *txn, const std::vector<RID> &rids,
                   const std::vector<std::pair<int32_t, std::string>> &rows) {
    auto chunk = ManyRows(rows);
    Vector rv{LogicalType{LogicalTypeId::BIGINT}};
    auto *d = FlatVector::GetData<int64_t>(rv);
    for (idx_t i = 0; i < rids.size(); i++) {
      d[i] = rids[i].Get();
    }
    MvccUpdate(&tm, txn, Oid(), Heap(), rv, chunk);
  }

  /** Delete a batch of rows under `txn` in a single MvccDelete call. */
  void DeleteBatch(Transaction *txn, const std::vector<RID> &rids) {
    Vector rv{LogicalType{LogicalTypeId::BIGINT}};
    auto *d = FlatVector::GetData<int64_t>(rv);
    for (idx_t i = 0; i < rids.size(); i++) {
      d[i] = rids[i].Get();
    }
    MvccDelete(&tm, txn, Oid(), Heap(), rv, rids.size());
  }

  void Update(Transaction *txn, RID rid, int32_t id, const std::string &name) {
    auto chunk = OneRow(id, name);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    FlatVector::GetData<int64_t>(rids)[0] = rid.Get();
    MvccUpdate(&tm, txn, Oid(), Heap(), rids, chunk);
  }

  void Delete(Transaction *txn, RID rid) {
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    FlatVector::GetData<int64_t>(rids)[0] = rid.Get();
    MvccDelete(&tm, txn, Oid(), Heap(), rids, 1);
  }

  /** Every (id, name) visible to `txn` via a visibility-filtered scan, in scan order. */
  auto ScanAll(Transaction *txn) -> std::vector<std::pair<int32_t, std::string>> {
    auto scan = Heap().MakeMvccScan(&tm, txn, Oid());
    DataChunk out;
    out.Initialize(table->schema_.GetTypes());
    std::vector<std::pair<int32_t, std::string>> rows;
    while (scan->Next(out)) {
      for (idx_t i = 0; i < out.GetSize(); i++) {
        rows.emplace_back(out.GetValue(0, i).GetAs<int32_t>(), out.GetValue(1, i).GetString());
      }
    }
    return rows;
  }

  /** The (id, name) visible to `txn` at `rid`, or nullopt if no version is visible. */
  auto Read(Transaction *txn, RID rid) -> std::optional<std::pair<int32_t, std::string>> {
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    FlatVector::GetData<int64_t>(rids)[0] = rid.Get();
    DataChunk out;
    out.Initialize(table->schema_.GetTypes());
    auto n = MvccFetch(&tm, txn, Heap(), rids, 1, out);
    if (n == 0) {
      return std::nullopt;
    }
    return std::make_pair(out.GetValue(0, 0).GetAs<int32_t>(), out.GetValue(1, 0).GetString());
  }
};

// A txn sees its own uncommitted insert and its own subsequent update.
TEST(MvccWriteTest, OwnWritesVisibleToSelf) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 1, "alice");
  auto v = f.Read(t1, rid);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->first, 1);
  EXPECT_EQ(v->second, "alice");

  f.Update(t1, rid, 1, "alICE");
  v = f.Read(t1, rid);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->second, "alICE");
}

// Another concurrent txn cannot see an uncommitted insert; it becomes visible only after commit.
TEST(MvccWriteTest, UncommittedInsertInvisibleUntilCommit) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 1, "alice");

  auto *other = f.tm.Begin();
  EXPECT_FALSE(f.Read(other, rid).has_value()) << "cannot see t1's uncommitted insert";
  f.tm.Commit(other);

  ASSERT_TRUE(f.tm.Commit(t1));
  auto *later = f.tm.Begin();
  auto v = f.Read(later, rid);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->second, "alice");
}

// Snapshot isolation: a reader that began before an update commits still sees the old version.
TEST(MvccWriteTest, SnapshotIsolationOldReaderSeesOldVersion) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 1, "v1");
  ASSERT_TRUE(f.tm.Commit(t1));

  auto *old_reader = f.tm.Begin();  // snapshot sees v1

  auto *t2 = f.tm.Begin();
  f.Update(t2, rid, 1, "v2");
  ASSERT_TRUE(f.tm.Commit(t2));

  auto *new_reader = f.tm.Begin();  // snapshot sees v2

  EXPECT_EQ(f.Read(old_reader, rid)->second, "v1") << "old snapshot still sees v1";
  EXPECT_EQ(f.Read(new_reader, rid)->second, "v2") << "new snapshot sees v2";
}

// First-committer-wins: two concurrent updaters of the same row — the second aborts on conflict.
TEST(MvccWriteTest, WriteWriteConflictSecondAborts) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 1, "v1");
  ASSERT_TRUE(f.tm.Commit(t1));

  auto *t2 = f.tm.Begin();
  auto *t3 = f.tm.Begin();
  f.Update(t2, rid, 1, "t2");  // t2 grabs the row (temp-stamps the base); same-length name

  EXPECT_THROW(f.Update(t3, rid, 1, "t3"), ExecutionException);
  EXPECT_EQ(t3->GetTransactionState(), TransactionState::TAINTED) << "conflicting writer is tainted";

  ASSERT_TRUE(f.tm.Commit(t2));
  f.tm.Abort(t3);

  auto *reader = f.tm.Begin();
  EXPECT_EQ(f.Read(reader, rid)->second, "t2") << "the winner's write is durable";
}

// Abort restores the committed pre-image (and the aborting txn saw its own change before aborting).
TEST(MvccWriteTest, AbortRestoresPreImage) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 1, "keep");
  ASSERT_TRUE(f.tm.Commit(t1));

  auto *t2 = f.tm.Begin();
  f.Update(t2, rid, 1, "drty");  // same-length name as "keep"
  EXPECT_EQ(f.Read(t2, rid)->second, "drty") << "t2 sees its own change";
  f.tm.Abort(t2);

  auto *reader = f.tm.Begin();
  EXPECT_EQ(f.Read(reader, rid)->second, "keep") << "abort rolled back to the pre-image";
}

// A fresh insert that aborts is invisible to everyone (tombstoned).
TEST(MvccWriteTest, AbortedInsertIsInvisible) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 9, "ghost");
  f.tm.Abort(t1);

  auto *reader = f.tm.Begin();
  EXPECT_FALSE(f.Read(reader, rid).has_value()) << "an aborted insert never becomes visible";
}

// A committed delete hides the row from later snapshots, but an older snapshot still sees it.
TEST(MvccWriteTest, DeleteHidesRowRespectingSnapshots) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rid = f.Insert(t1, 1, "doomed");
  ASSERT_TRUE(f.tm.Commit(t1));

  auto *old_reader = f.tm.Begin();  // sees the row

  auto *t2 = f.tm.Begin();
  f.Delete(t2, rid);
  ASSERT_TRUE(f.tm.Commit(t2));

  auto *new_reader = f.tm.Begin();
  EXPECT_FALSE(f.Read(new_reader, rid).has_value()) << "deleted for later snapshots";
  ASSERT_TRUE(f.Read(old_reader, rid).has_value()) << "still visible to the older snapshot";
  EXPECT_EQ(f.Read(old_reader, rid)->second, "doomed");
}

// A size-changing update (var-length column) is versioned correctly: the writer and later snapshots
// see the resized value, an older snapshot still reconstructs the original size, and a neighbouring
// row on the same page is preserved through the compaction. Also covers grow-then-shrink and abort.
TEST(MvccWriteTest, SizeChangingUpdateIsVersioned) {
  MvccFixture f;
  auto *seed = f.tm.Begin();
  auto a = f.Insert(seed, 1, "ab");  // short
  auto neighbour = f.Insert(seed, 2, "keepme");
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *old_reader = f.tm.Begin();  // sees a = "ab"

  auto *t2 = f.tm.Begin();
  f.Update(t2, a, 1, "a-much-longer-value");  // grow
  ASSERT_TRUE(f.tm.Commit(t2));

  auto *new_reader = f.tm.Begin();
  EXPECT_EQ(f.Read(old_reader, a)->second, "ab") << "old snapshot reconstructs the original size";
  EXPECT_EQ(f.Read(new_reader, a)->second, "a-much-longer-value") << "new snapshot sees the grown value";
  EXPECT_EQ(f.Read(new_reader, neighbour)->second, "keepme") << "neighbour survived the compaction";

  // Shrink it back, then abort a further grow — the committed shrunk value must remain.
  auto *t3 = f.tm.Begin();
  f.Update(t3, a, 1, "z");  // shrink
  ASSERT_TRUE(f.tm.Commit(t3));

  auto *t4 = f.tm.Begin();
  f.Update(t4, a, 1, "grown-then-rolled-back");
  f.tm.Abort(t4);

  auto *final_reader = f.tm.Begin();
  EXPECT_EQ(f.Read(final_reader, a)->second, "z") << "aborted resize rolled back to the shrunk value";
  EXPECT_EQ(f.Read(final_reader, neighbour)->second, "keepme");
}

// A visibility-filtered scan returns each row's snapshot version: an older reader sees the pre-update
// values and the not-yet-deleted row and none of the later insert; a newer reader sees all the changes.
TEST(MvccWriteTest, ScanReflectsEachSnapshot) {
  MvccFixture f;
  auto *seed = f.tm.Begin();
  auto r1 = f.Insert(seed, 1, "aaaa");
  auto r2 = f.Insert(seed, 2, "bbbb");
  f.Insert(seed, 3, "cccc");
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *old_reader = f.tm.Begin();  // snapshot: {1:aaaa, 2:bbbb, 3:cccc}

  auto *w = f.tm.Begin();
  f.Update(w, r1, 1, "AAAA");  // same length
  f.Delete(w, r2);
  f.Insert(w, 4, "dddd");
  ASSERT_TRUE(f.tm.Commit(w));

  auto *new_reader = f.tm.Begin();  // snapshot: {1:AAAA, 3:cccc, 4:dddd}

  auto old_rows = f.ScanAll(old_reader);
  ASSERT_EQ(old_rows.size(), 3U);
  EXPECT_EQ(old_rows[0], std::make_pair(1, std::string("aaaa")));
  EXPECT_EQ(old_rows[1], std::make_pair(2, std::string("bbbb")));
  EXPECT_EQ(old_rows[2], std::make_pair(3, std::string("cccc")));

  auto new_rows = f.ScanAll(new_reader);
  ASSERT_EQ(new_rows.size(), 3U);
  EXPECT_EQ(new_rows[0], std::make_pair(1, std::string("AAAA")));  // updated in place, same RID
  EXPECT_EQ(new_rows[1], std::make_pair(3, std::string("cccc")));  // row 2 deleted
  EXPECT_EQ(new_rows[2], std::make_pair(4, std::string("dddd")));  // later insert now visible
}

// A scan under a snapshot never surfaces another txn's uncommitted insert.
TEST(MvccWriteTest, ScanSkipsUncommittedRows) {
  MvccFixture f;
  auto *seed = f.tm.Begin();
  f.Insert(seed, 1, "real");
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *writer = f.tm.Begin();
  f.Insert(writer, 2, "wip");  // uncommitted

  auto *reader = f.tm.Begin();
  auto rows = f.ScanAll(reader);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].second, "real") << "the uncommitted row is not scanned";

  ASSERT_TRUE(f.tm.Commit(writer));
  auto *later = f.tm.Begin();
  EXPECT_EQ(f.ScanAll(later).size(), 2U) << "visible after commit";
}

// A single MvccInsert/MvccUpdate/MvccDelete call operating on a multi-row DataChunk versions every
// row in the batch, not just the first.
TEST(MvccWriteTest, BatchInsertUpdateDelete) {
  MvccFixture f;
  auto *t1 = f.tm.Begin();
  auto rids = f.InsertBatch(t1, {{1, "a"}, {2, "b"}, {3, "c"}});
  ASSERT_EQ(rids.size(), 3U);
  ASSERT_TRUE(f.tm.Commit(t1));

  auto *r1 = f.tm.Begin();
  EXPECT_EQ(f.ScanAll(r1).size(), 3U);

  // Batch update all three, then commit.
  auto *t2 = f.tm.Begin();
  f.UpdateBatch(t2, rids, {{1, "AA"}, {2, "BB"}, {3, "CC"}});
  ASSERT_TRUE(f.tm.Commit(t2));
  auto *r2 = f.tm.Begin();
  auto rows = f.ScanAll(r2);
  ASSERT_EQ(rows.size(), 3U);
  for (auto &row : rows) {
    EXPECT_EQ(row.second.size(), 2U) << "every row in the batch was updated";
  }

  // Batch delete the first two; the third survives.
  auto *t3 = f.tm.Begin();
  f.DeleteBatch(t3, {rids[0], rids[1]});
  ASSERT_TRUE(f.tm.Commit(t3));
  auto *r3 = f.tm.Begin();
  auto after = f.ScanAll(r3);
  ASSERT_EQ(after.size(), 1U);
  EXPECT_EQ(after[0].first, 3);

  // The original snapshot still sees all three unmodified.
  EXPECT_EQ(f.ScanAll(r1).size(), 3U);
}

// A version chain several updates deep: a reader pinned at each historical snapshot must reconstruct
// exactly the version that was current then, walking as many undo logs as needed.
TEST(MvccWriteTest, DeepVersionChain) {
  MvccFixture f;
  auto *t0 = f.tm.Begin();
  auto rid = f.Insert(t0, 1, "v0");
  ASSERT_TRUE(f.tm.Commit(t0));

  std::vector<Transaction *> readers;
  std::vector<std::string> expected;
  readers.push_back(f.tm.Begin());  // snapshot after v0
  expected.emplace_back("v0");

  // Apply four more committed versions, capturing a snapshot after each. Sizes differ deliberately.
  const std::vector<std::string> values{"version-1", "v2", "the-third-value", "4444"};
  for (const auto &v : values) {
    auto *w = f.tm.Begin();
    f.Update(w, rid, 1, v);
    ASSERT_TRUE(f.tm.Commit(w));
    readers.push_back(f.tm.Begin());
    expected.push_back(v);
  }

  // Each reader reconstructs its own snapshot's version out of the 5-deep chain.
  for (size_t i = 0; i < readers.size(); i++) {
    auto got = f.Read(readers[i], rid);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->second, expected[i]) << "reader " << i << " must see its snapshot version";
  }
}

// MVCC across a table that spans several pages: an older snapshot sees the whole original table while
// updates and deletes committed on later pages are invisible to it and visible to a newer snapshot.
TEST(MvccWriteTest, MultiPageVisibility) {
  MvccFixture f;
  constexpr int kRows = 600;
  std::vector<std::pair<int32_t, std::string>> seed_rows;
  seed_rows.reserve(kRows);
  for (int i = 0; i < kRows; i++) {
    seed_rows.emplace_back(i, "r" + std::to_string(i));
  }
  auto *t0 = f.tm.Begin();
  auto rids = f.InsertBatch(t0, seed_rows);
  ASSERT_TRUE(f.tm.Commit(t0));
  ASSERT_NE(f.Heap().GetFirstPageId(), f.Heap().GetLastPageId()) << "the table must span multiple pages";

  auto *old_reader = f.tm.Begin();  // sees all 600 originals

  // Update every 50th row and delete every 70th (spread across pages), then commit.
  auto *t1 = f.tm.Begin();
  int updated = 0;
  int deleted = 0;
  for (int i = 0; i < kRows; i++) {
    if (i % 50 == 0) {
      f.Update(t1, rids[i], i, "U");  // shrink to a short marker (always fits a packed page)
      updated++;
    } else if (i % 70 == 0) {
      f.Delete(t1, rids[i]);
      deleted++;
    }
  }
  ASSERT_TRUE(f.tm.Commit(t1));

  auto *new_reader = f.tm.Begin();
  auto new_rows = f.ScanAll(new_reader);
  EXPECT_EQ(new_rows.size(), static_cast<size_t>(kRows - deleted)) << "deletes removed rows for the new snapshot";
  int updated_seen = 0;
  for (auto &row : new_rows) {
    if (row.second == "U") {
      updated_seen++;
    }
  }
  EXPECT_EQ(updated_seen, updated) << "all updates visible to the new snapshot";

  // The older snapshot is unaffected: it still sees all 600 originals, none updated or deleted.
  auto old_rows = f.ScanAll(old_reader);
  EXPECT_EQ(old_rows.size(), static_cast<size_t>(kRows));
  for (auto &row : old_rows) {
    EXPECT_NE(row.second, "U") << "the old snapshot sees no later update";
  }
}

// Many threads, each in its own txn begun from the same snapshot, race to update the same row. The
// first to temp-stamp the base wins; every other sees the temp stamp and conflicts. Exactly one
// commits — and the whole thing is data-race free under ThreadSanitizer.
TEST(MvccWriteTest, ConcurrentWriteWriteExactlyOneWins) {
  MvccFixture f;
  auto *seed = f.tm.Begin();
  auto rid = f.Insert(seed, 1, "seed");
  ASSERT_TRUE(f.tm.Commit(seed));

  constexpr int kThreads = 8;
  // Begin all txns up front so they share one snapshot (all conflict against the first writer).
  std::vector<Transaction *> txns;
  txns.reserve(kThreads);
  for (int i = 0; i < kThreads; i++) {
    txns.push_back(f.tm.Begin());
  }

  std::atomic<int> committed{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; i++) {
    threads.emplace_back([&f, &committed, rid, txn = txns[i]]() {
      try {
        f.Update(txn, rid, 1, "wonn");  // 4 chars, same size as "seed"
        if (f.tm.Commit(txn)) {
          committed.fetch_add(1);
        }
      } catch (const ExecutionException &) {
        f.tm.Abort(txn);  // write-write conflict — this loser rolls back
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  EXPECT_EQ(committed.load(), 1) << "exactly one concurrent writer commits";

  auto *reader = f.tm.Begin();
  EXPECT_EQ(f.Read(reader, rid)->second, "wonn") << "the winner's value is durable";
}

}  // namespace bumblebee
