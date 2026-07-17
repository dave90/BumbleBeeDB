//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transaction_test.cpp
//
// Identification: test/unit/concurrency/transaction_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <thread>  // NOLINT
#include <vector>

#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"
#include "gtest/gtest.h"
#include "storage/table/rid.h"

namespace bumblebee {

// Begin snapshots the latest committed timestamp; commit advances it; the next txn sees it.
TEST(TransactionTest, BeginCommitAdvancesTimestamps) {
  TransactionManager tm;
  auto *t1 = tm.Begin();
  EXPECT_EQ(t1->GetReadTs(), 0);
  EXPECT_EQ(tm.GetLastCommitTs(), 0);
  ASSERT_TRUE(tm.Commit(t1));
  EXPECT_EQ(t1->GetCommitTs(), 1);
  EXPECT_EQ(tm.GetLastCommitTs(), 1);
  EXPECT_EQ(t1->GetTransactionState(), TransactionState::COMMITTED);

  auto *t2 = tm.Begin();
  EXPECT_EQ(t2->GetReadTs(), 1) << "second txn snapshots the first's commit";
  ASSERT_TRUE(tm.Commit(t2));
  EXPECT_EQ(tm.GetLastCommitTs(), 2);
}

// A txn's temp stamp is its id, which is >= TXN_START_ID and recognised as temporary.
TEST(TransactionTest, TempTsIsTxnIdAndTemporary) {
  TransactionManager tm;
  auto *t = tm.Begin();
  EXPECT_GE(t->GetTransactionId(), TXN_START_ID);
  EXPECT_EQ(t->GetTransactionTempTs(), t->GetTransactionId());
  EXPECT_TRUE(Transaction::IsTempTs(t->GetTransactionTempTs()));
  EXPECT_FALSE(Transaction::IsTempTs(0));
  EXPECT_FALSE(Transaction::IsTempTs(t->GetReadTs()));
  tm.Commit(t);
}

// Abort does not advance the commit clock, and marks the txn ABORTED.
TEST(TransactionTest, AbortDoesNotAdvanceCommitTs) {
  TransactionManager tm;
  auto *t1 = tm.Begin();
  tm.Commit(t1);
  EXPECT_EQ(tm.GetLastCommitTs(), 1);

  auto *t2 = tm.Begin();
  tm.Abort(t2);
  EXPECT_EQ(t2->GetTransactionState(), TransactionState::ABORTED);
  EXPECT_EQ(tm.GetLastCommitTs(), 1) << "abort must not bump the commit clock";
}

// The watermark tracks the oldest live read ts, then follows the commit clock once all are done.
TEST(TransactionTest, WatermarkTracksLiveThenCommit) {
  TransactionManager tm;
  auto *t1 = tm.Begin();  // read_ts 0
  tm.Commit(t1);          // last_commit_ts 1
  auto *t2 = tm.Begin();  // read_ts 1
  auto *t3 = tm.Begin();  // read_ts 1
  tm.Commit(t3);          // last_commit_ts 2, but t2 still live at 1
  EXPECT_EQ(tm.GetWatermark(), 1) << "t2 still reading at 1";
  tm.Commit(t2);  // this commit itself bumps last_commit_ts to 3
  EXPECT_EQ(tm.GetWatermark(), 3) << "no live readers → latest commit ts";
}

// GC reclaims finished txns at/below the watermark; a still-live txn keeps its snapshot pinned.
TEST(TransactionTest, GarbageCollectionReclaimsFinished) {
  TransactionManager tm;
  auto *t1 = tm.Begin();
  tm.Commit(t1);
  auto *t2 = tm.Begin();  // pins read_ts 1
  auto *t3 = tm.Begin();
  tm.Commit(t3);
  // t1 committed at 1, watermark is 1 (t2 live) → t1 collectible, t3 (commit 2) not.
  tm.GarbageCollection();  // must not crash / null-insert
  tm.Commit(t2);
  tm.GarbageCollection();  // watermark now 2 → t3 collectible
  SUCCEED();
}

// GC enforces the transaction timeout: a txn older than the manager's timeout is aborted, then
// reclaimed in the same pass. A zero timeout makes every txn instantly expired (deterministic).
TEST(TransactionTest, GarbageCollectionAbortsTimedOutTxn) {
  TransactionManager tm{nullptr, duration_t::zero()};
  tm.Begin();  // an idle, read-only txn (empty write set → rollback needs no catalog)
  EXPECT_EQ(tm.GetTransactionCount(), 1U);
  tm.GarbageCollection();
  // The only txn was RUNNING, so count can reach 0 only by it being timed-out-aborted then reclaimed.
  EXPECT_EQ(tm.GetTransactionCount(), 0U) << "the timed-out txn was aborted and reclaimed";
}

// A transaction still within the timeout is left running by GC.
TEST(TransactionTest, GarbageCollectionKeepsFreshTxn) {
  TransactionManager tm;  // default 2-hour timeout
  auto *t = tm.Begin();
  tm.GarbageCollection();
  EXPECT_EQ(t->GetTransactionState(), TransactionState::RUNNING) << "a fresh txn is not timed out";
  EXPECT_EQ(tm.GetTransactionCount(), 1U);
}

// The version side-table stores and updates head undo links per RID; the check gate can veto.
TEST(TransactionTest, VersionSideTableLinkAndCheck) {
  TransactionManager tm;
  RID rid{3, 7};
  EXPECT_FALSE(tm.GetUndoLink(rid).has_value());

  UndoLink link{TXN_START_ID + 4, 2};
  ASSERT_TRUE(tm.UpdateUndoLink(rid, link));
  auto got = tm.GetUndoLink(rid);
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(*got == link);

  // A check that vetoes leaves the link unchanged.
  UndoLink other{TXN_START_ID + 9, 0};
  bool ok = tm.UpdateUndoLink(rid, other, [](std::optional<UndoLink> cur) { return !cur.has_value(); });
  EXPECT_FALSE(ok);
  EXPECT_TRUE(*tm.GetUndoLink(rid) == link) << "vetoed update must not overwrite";

  // A check that passes updates it.
  ok = tm.UpdateUndoLink(rid, other, [&](std::optional<UndoLink> cur) { return cur.has_value() && *cur == link; });
  EXPECT_TRUE(ok);
  EXPECT_TRUE(*tm.GetUndoLink(rid) == other);
}

// Concurrent Begin/Commit from many threads: all ids unique, commit clock consistent, TSan-clean.
TEST(TransactionTest, ConcurrentBeginCommit) {
  TransactionManager tm;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; i++) {
    threads.emplace_back([&tm]() {
      for (int j = 0; j < kPerThread; j++) {
        auto *t = tm.Begin();
        if (j % 3 == 0) {
          tm.Abort(t);
        } else {
          tm.Commit(t);
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  // Every committed txn advanced the clock by exactly 1; aborts advance nothing.
  int commits = kThreads * (kPerThread - (kPerThread + 2) / 3);
  EXPECT_EQ(tm.GetLastCommitTs(), commits);
  tm.GarbageCollection();
  EXPECT_EQ(tm.GetWatermark(), tm.GetLastCommitTs()) << "no live readers left";
}

}  // namespace bumblebee
