//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// mvcc_stress_test.cpp
//
// Identification: test/unit/concurrency/mvcc_stress_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <memory>
#include <optional>
#include <random>
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

auto ReadIntColumn(const RowLayout &layout, const char *row) -> int32_t {
  return *reinterpret_cast<const int32_t *>(row + layout.GetOffsets()[0]);
}

/** A one-column (INTEGER v) MVCC table — integer values keep every update the same byte size. */
struct IntFixture {
  MemoryDiskManager dm{1024};
  BufferPoolManager bpm{64, &dm};
  Catalog catalog{&bpm};
  TransactionManager tm{&catalog};
  std::shared_ptr<TableInfo> table;

  IntFixture() {
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

  auto ReadValue(Transaction *txn, RID rid) -> std::optional<int32_t> {
    auto bytes = CollectVisibleVersion(&tm, txn, Heap(), rid);
    if (!bytes.has_value()) {
      return std::nullopt;
    }
    return ReadIntColumn(Heap().GetLayout(), bytes->data());
  }
};

}  // namespace

// GC reclaims committed txns once no live snapshot needs their versions; a long reader pins them.
TEST(MvccStressTest, GarbageCollectionRespectsLongReader) {
  IntFixture f;
  auto *seed = f.tm.Begin();
  auto rid = f.Insert(seed, 0);
  ASSERT_TRUE(f.tm.Commit(seed));

  auto *long_reader = f.tm.Begin();  // pins the watermark at read_ts 1
  ASSERT_EQ(f.ReadValue(long_reader, rid).value(), 0);

  // A chain of committed updates, each leaving an undo log the long reader still needs to walk.
  for (int i = 1; i <= 5; i++) {
    auto *w = f.tm.Begin();
    f.Update(w, rid, i);
    ASSERT_TRUE(f.tm.Commit(w));
  }

  f.tm.GarbageCollection();
  // The long reader still sees the original value by walking the (retained) version chain.
  EXPECT_EQ(f.ReadValue(long_reader, rid).value(), 0) << "old snapshot intact after GC";
  EXPECT_GT(f.tm.GetTransactionCount(), 1U) << "versions needed by the long reader are retained";

  ASSERT_TRUE(f.tm.Commit(long_reader));  // watermark advances to the latest commit
  f.tm.GarbageCollection();
  EXPECT_EQ(f.tm.GetTransactionCount(), 0U) << "no live snapshot → every finished txn reclaimed";

  // The table is still correct: a fresh snapshot sees the latest committed value.
  auto *reader = f.tm.Begin();
  EXPECT_EQ(f.ReadValue(reader, rid).value(), 5) << "latest version survives GC (it is the base row)";
}

// End-to-end money-transfer stress: many threads move value between accounts under MVCC. Write-write
// conflict detection prevents lost updates, so the total is conserved and no balance goes negative.
// The whole run must be data-race free under ThreadSanitizer.
TEST(MvccStressTest, ConcurrentTransfersConserveTotal) {
  IntFixture f;
  constexpr int kAccounts = 8;
  constexpr int kStartBalance = 1000;
  constexpr int kThreads = 8;
  constexpr int kTransfersPerThread = 300;

  std::vector<RID> accounts;
  auto *seed = f.tm.Begin();
  for (int i = 0; i < kAccounts; i++) {
    accounts.push_back(f.Insert(seed, kStartBalance));
  }
  ASSERT_TRUE(f.tm.Commit(seed));
  const int64_t expected_total = static_cast<int64_t>(kAccounts) * kStartBalance;

  std::atomic<int> committed{0};
  std::atomic<int> aborted{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&f, &accounts, &committed, &aborted, t]() {
      std::mt19937 rng(1234 + t);  // deterministic per-thread seed (no wall-clock needed)
      std::uniform_int_distribution<int> pick(0, kAccounts - 1);
      std::uniform_int_distribution<int> amt(1, 50);
      for (int k = 0; k < kTransfersPerThread; k++) {
        int i = pick(rng);
        int j = pick(rng);
        if (i == j) {
          continue;
        }
        int amount = amt(rng);
        auto *txn = f.tm.Begin();
        try {
          auto bi = f.ReadValue(txn, accounts[i]);
          auto bj = f.ReadValue(txn, accounts[j]);
          if (!bi.has_value() || !bj.has_value() || bi.value() < amount) {
            f.tm.Abort(txn);
            aborted.fetch_add(1);
            continue;
          }
          f.Update(txn, accounts[i], bi.value() - amount);
          f.Update(txn, accounts[j], bj.value() + amount);
          if (f.tm.Commit(txn)) {
            committed.fetch_add(1);
          } else {
            aborted.fetch_add(1);
          }
        } catch (const ExecutionException &) {
          f.tm.Abort(txn);  // write-write conflict — this transfer is rolled back
          aborted.fetch_add(1);
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  // Total conserved and every balance non-negative, read under a fresh snapshot.
  auto *auditor = f.tm.Begin();
  int64_t total = 0;
  for (int i = 0; i < kAccounts; i++) {
    auto b = f.ReadValue(auditor, accounts[i]);
    ASSERT_TRUE(b.has_value());
    EXPECT_GE(b.value(), 0) << "account " << i << " went negative";
    total += b.value();
  }
  EXPECT_EQ(total, expected_total) << "MVCC conflict detection conserved the total";
  EXPECT_GT(committed.load(), 0) << "some transfers committed";
}

}  // namespace bumblebee
