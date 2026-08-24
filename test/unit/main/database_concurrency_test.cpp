//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database_concurrency_test.cpp
//
// Identification: test/unit/main/database_concurrency_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/database_instance.h"

#include <atomic>
#include <barrier>
#include <chrono>  // NOLINT
#include <filesystem>
#include <future>  // NOLINT
#include <memory>
#include <sstream>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "common/exception.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/query_memory_manager.h"
#include "main/query_result.h"

namespace bumblebee {

namespace {

using namespace std::chrono_literals;

auto ExecuteOne(Connection &connection, const std::string &sql) -> QueryResult {
  auto results = connection.ExecuteSqlResults(sql);
  if (results.size() != 1) {
    throw std::runtime_error("expected exactly one result");
  }
  return std::move(results.front());
}

auto ScalarInt(Connection &connection, const std::string &sql) -> int64_t {
  auto rows = ExecuteOne(connection, sql).MaterializeRows();
  if (rows.size() != 1 || rows.front().size() != 1) {
    throw std::runtime_error("expected one scalar row");
  }
  return rows.front().front().GetAs<int64_t>();
}

auto ValuesSql(idx_t count) -> std::string {
  std::ostringstream out;
  out << "INSERT INTO t VALUES ";
  for (idx_t i = 0; i < count; i++) {
    if (i > 0) {
      out << ',';
    }
    out << '(' << i << ',' << i * 10 << ')';
  }
  out << ';';
  return out.str();
}

template <class Predicate>
auto SpinUntil(Predicate predicate) -> bool {
  for (size_t i = 0; i < 2'000'000; i++) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

auto TempDatabasePath(const std::string &suffix) -> std::filesystem::path {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("bumblebee_phase3_" + suffix + "_" + std::to_string(unique) + ".db");
}

}  // namespace

TEST(DatabaseConcurrencyTest, OperationTokenMakesCloseWaitAndRejectsNewWork) {
  auto database = std::make_shared<DatabaseInstance>();
  auto token = DatabaseInstance::AcquireOperation(database);
  auto close = std::async(std::launch::async, [&] { database->Close(); });

  ASSERT_TRUE(SpinUntil([&] { return database->State() == DatabaseInstance::LifecycleState::CLOSING; }));
  EXPECT_THROW(static_cast<void>(DatabaseInstance::AcquireOperation(database)), DatabaseClosedException);
  EXPECT_EQ(close.wait_for(1ms), std::future_status::timeout);

  token = {};
  EXPECT_EQ(close.wait_for(5s), std::future_status::ready);
  close.get();
  EXPECT_EQ(database->State(), DatabaseInstance::LifecycleState::CLOSED);
}

TEST(DatabaseConcurrencyTest, RacingDatabaseCloseCallsAreIdempotent) {
  auto database = std::make_shared<DatabaseInstance>();
  std::barrier start(3);
  auto first = std::async(std::launch::async, [&] {
    start.arrive_and_wait();
    database->Close();
  });
  auto second = std::async(std::launch::async, [&] {
    start.arrive_and_wait();
    database->Close();
  });
  start.arrive_and_wait();
  first.get();
  second.get();
  EXPECT_EQ(database->State(), DatabaseInstance::LifecycleState::CLOSED);
}

TEST(DatabaseConcurrencyTest, OneConnectionRejectsConcurrentStatementsDeterministically) {
  DatabaseConfig config;
  config.worker_threads_ = 1;
  auto database = std::make_shared<DatabaseInstance>(config);
  auto connection = DatabaseInstance::CreateConnection(database);

  auto occupied_slot = database->AcquireWorkerSlot();
  auto first = std::async(std::launch::async, [&] { return ScalarInt(*connection, "SELECT 42;"); });
  ASSERT_TRUE(SpinUntil([&] { return database->ActiveOperationCount() == 1; }));
  EXPECT_THROW(static_cast<void>(connection->ExecuteSqlResults("SELECT 7;")), ProgrammingException);

  occupied_slot = {};
  EXPECT_EQ(first.get(), 42);
  EXPECT_EQ(database->ActiveWorkerSlots(), 0U);
}

TEST(DatabaseConcurrencyTest, ConnectionCloseWaitsForItsStatementThenInvalidatesTheSession) {
  DatabaseConfig config;
  config.worker_threads_ = 1;
  auto database = std::make_shared<DatabaseInstance>(config);
  auto connection = DatabaseInstance::CreateConnection(database);

  auto occupied_slot = database->AcquireWorkerSlot();
  auto query = std::async(std::launch::async, [&] { return ScalarInt(*connection, "SELECT 99;"); });
  ASSERT_TRUE(SpinUntil([&] { return database->ActiveOperationCount() == 1; }));
  auto close = std::async(std::launch::async, [&] { connection->Close(); });
  EXPECT_EQ(close.wait_for(1ms), std::future_status::timeout);

  occupied_slot = {};
  EXPECT_EQ(query.get(), 99);
  close.get();
  EXPECT_TRUE(connection->IsClosed());
  EXPECT_THROW(static_cast<void>(connection->ExecuteSqlResults("SELECT 1;")), ProgrammingException);
}

TEST(DatabaseConcurrencyTest, SchemaLeaseBlocksDropAndDdlIsRejectedInsideTransaction) {
  auto database = std::make_shared<DatabaseInstance>();
  auto setup = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(v INT);");

  auto lease = database->AcquireSharedSchemaLease();
  auto ddl = DatabaseInstance::CreateConnection(database);
  auto drop = std::async(std::launch::async, [&] { return ExecuteOne(*ddl, "DROP TABLE t;"); });
  EXPECT_EQ(drop.wait_for(1ms), std::future_status::timeout);
  lease.unlock();
  drop.get();
  EXPECT_EQ(database->GetCatalog().GetTable("t"), NULL_TABLE_INFO);

  ExecuteOne(*setup, "CREATE TABLE t(v INT);");
  ExecuteOne(*setup, "BEGIN;");
  EXPECT_THROW(static_cast<void>(setup->ExecuteSqlResults("DROP TABLE t;")), ProgrammingException);
  EXPECT_TRUE(setup->HasActiveTransaction());
  ExecuteOne(*setup, "ROLLBACK;");
}

TEST(DatabaseConcurrencyTest, ShutdownReleasesIdleTransactionLeaseBeforeDrainingBlockedDdl) {
  auto database = std::make_shared<DatabaseInstance>();
  auto setup = DatabaseInstance::CreateConnection(database);
  auto transaction = DatabaseInstance::CreateConnection(database);
  auto ddl = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(v INT);");
  ExecuteOne(*transaction, "BEGIN;");

  auto drop = std::async(std::launch::async, [&] { return ExecuteOne(*ddl, "DROP TABLE t;"); });
  ASSERT_TRUE(SpinUntil([&] { return database->ActiveOperationCount() == 1; }));
  EXPECT_EQ(drop.wait_for(1ms), std::future_status::timeout);

  auto close = std::async(std::launch::async, [&] { database->Close(); });
  const bool completed = close.wait_for(5s) == std::future_status::ready;
  if (!completed) {
    // Keep a regression failure from leaving the test process blocked in std::future destruction.
    transaction->Close();
    static_cast<void>(close.wait_for(5s));
    static_cast<void>(drop.wait_for(5s));
  }
  ASSERT_TRUE(completed) << "database close waited behind DDL and an idle transaction schema lease";
  close.get();
  ASSERT_EQ(drop.wait_for(5s), std::future_status::ready);
  static_cast<void>(drop.get());
  EXPECT_TRUE(setup->IsClosed());
  EXPECT_TRUE(transaction->IsClosed());
  EXPECT_TRUE(ddl->IsClosed());
  EXPECT_EQ(database->State(), DatabaseInstance::LifecycleState::CLOSED);
}

TEST(DatabaseConcurrencyTest, SeparateConnectionsRunWholeSelectAggregateAndJoinQueries) {
  DatabaseConfig config;
  config.worker_threads_ = 4;
  auto database = std::make_shared<DatabaseInstance>(config);
  auto setup = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(id INT PRIMARY KEY, v INT);");
  ExecuteOne(*setup, ValuesSql(500));

  constexpr size_t kCallers = 4;
  std::barrier start(static_cast<std::ptrdiff_t>(kCallers));
  std::vector<std::future<int64_t>> calls;
  for (size_t i = 0; i < kCallers; i++) {
    calls.push_back(std::async(std::launch::async, [&, connection = DatabaseInstance::CreateConnection(database)] {
      start.arrive_and_wait();
      return ScalarInt(*connection, "SELECT SUM(a.v + b.v) FROM t a, t b;");
    }));
  }
  for (auto &call : calls) {
    EXPECT_EQ(call.get(), 1'247'500'000LL);
  }
  EXPECT_LE(database->PeakWorkerSlots(), database->WorkerCapacity());
  EXPECT_GE(database->PeakWorkerSlots(), 2U);
  EXPECT_EQ(database->ActiveWorkerSlots(), 0U);
}

TEST(DatabaseConcurrencyTest, ConcurrentReadInsertAndDisjointUpdatesRemainConsistent) {
  DatabaseConfig config;
  config.worker_threads_ = 3;
  auto database = std::make_shared<DatabaseInstance>(config);
  auto setup = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(id INT PRIMARY KEY, v INT);");
  ExecuteOne(*setup, ValuesSql(100));

  std::barrier read_write_start(2);
  auto reader = std::async(std::launch::async, [&, connection = DatabaseInstance::CreateConnection(database)] {
    read_write_start.arrive_and_wait();
    return ScalarInt(*connection, "SELECT COUNT(*) FROM t;");
  });
  auto inserter = std::async(std::launch::async, [&, connection = DatabaseInstance::CreateConnection(database)] {
    read_write_start.arrive_and_wait();
    return ExecuteOne(*connection, "INSERT INTO t VALUES (100, 1000);").AffectedRows();
  });
  const auto observed = reader.get();
  EXPECT_TRUE(observed == 100 || observed == 101);
  ASSERT_TRUE(inserter.get().has_value());
  EXPECT_EQ(ScalarInt(*setup, "SELECT COUNT(*) FROM t;"), 101);

  std::barrier update_start(2);
  auto first = std::async(std::launch::async, [&, connection = DatabaseInstance::CreateConnection(database)] {
    update_start.arrive_and_wait();
    ExecuteOne(*connection, "UPDATE t SET v = v + 1 WHERE id = 1;");
  });
  auto second = std::async(std::launch::async, [&, connection = DatabaseInstance::CreateConnection(database)] {
    update_start.arrive_and_wait();
    ExecuteOne(*connection, "UPDATE t SET v = v + 2 WHERE id = 2;");
  });
  first.get();
  second.get();
  EXPECT_EQ(ScalarInt(*setup, "SELECT SUM(v) FROM t WHERE id = 1 OR id = 2;"), 33);
}

TEST(DatabaseConcurrencyTest, SameRowAndPrimaryKeyConflictsHaveExactlyOneWinner) {
  auto database = std::make_shared<DatabaseInstance>();
  auto setup = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(id INT PRIMARY KEY, v INT);");
  ExecuteOne(*setup, "INSERT INTO t VALUES (1, 0);");

  auto first = DatabaseInstance::CreateConnection(database);
  auto second = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*first, "BEGIN;");
  ExecuteOne(*second, "BEGIN;");
  std::barrier update_start(2);
  std::atomic<int> update_winners{0};
  auto update = [&](const std::shared_ptr<Connection> &connection, int value) {
    update_start.arrive_and_wait();
    try {
      ExecuteOne(*connection, "UPDATE t SET v = " + std::to_string(value) + " WHERE id = 1;");
      ExecuteOne(*connection, "COMMIT;");
      update_winners.fetch_add(1, std::memory_order_relaxed);
    } catch (const Exception &) {
      if (connection->HasActiveTransaction()) {
        ExecuteOne(*connection, "ROLLBACK;");
      }
    }
  };
  auto update_one = std::async(std::launch::async, update, first, 10);
  auto update_two = std::async(std::launch::async, update, second, 20);
  update_one.get();
  update_two.get();
  EXPECT_EQ(update_winners.load(), 1);
  const auto final_value = ScalarInt(*setup, "SELECT v FROM t WHERE id = 1;");
  EXPECT_TRUE(final_value == 10 || final_value == 20);

  auto insert_one = DatabaseInstance::CreateConnection(database);
  auto insert_two = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*insert_one, "BEGIN;");
  ExecuteOne(*insert_two, "BEGIN;");
  std::barrier insert_start(2);
  std::atomic<int> insert_winners{0};
  auto insert = [&](const std::shared_ptr<Connection> &connection, int value) {
    insert_start.arrive_and_wait();
    try {
      ExecuteOne(*connection, "INSERT INTO t VALUES (2, " + std::to_string(value) + ");");
      ExecuteOne(*connection, "COMMIT;");
      insert_winners.fetch_add(1, std::memory_order_relaxed);
    } catch (const Exception &) {
      if (connection->HasActiveTransaction()) {
        ExecuteOne(*connection, "ROLLBACK;");
      }
    }
  };
  auto insert_first = std::async(std::launch::async, insert, insert_one, 30);
  auto insert_second = std::async(std::launch::async, insert, insert_two, 40);
  insert_first.get();
  insert_second.get();
  EXPECT_EQ(insert_winners.load(), 1);
  EXPECT_EQ(ScalarInt(*setup, "SELECT COUNT(*) FROM t WHERE id = 2;"), 1);
}

TEST(DatabaseConcurrencyTest, TimeoutGcCancelsActiveTransactionWithoutConcurrentRollback) {
  TransactionManager manager(nullptr, duration_t::zero());
  auto transaction = manager.BeginShared();
  transaction->EnterStatement();

  const auto first = manager.GarbageCollection();
  EXPECT_EQ(first.timed_out_, 1U);
  EXPECT_TRUE(transaction->IsCancellationRequested());
  EXPECT_EQ(transaction->GetTransactionState(), TransactionState::RUNNING);
  EXPECT_EQ(manager.GetTransactionCount(), 1U);

  transaction->LeaveStatement();
  const auto second = manager.GarbageCollection();
  EXPECT_EQ(second.timed_out_, 0U);
  EXPECT_EQ(transaction->GetTransactionState(), TransactionState::ABORTED);
  EXPECT_EQ(manager.GetTransactionCount(), 0U);
}

TEST(DatabaseConcurrencyTest, DatabaseLevelGcReconcilesTimedOutTransactionOwnerAndSchemaLease) {
  DatabaseConfig config;
  config.transaction_timeout_ = duration_t::zero();
  auto database = std::make_shared<DatabaseInstance>(config);
  auto setup = DatabaseInstance::CreateConnection(database);
  auto owner = DatabaseInstance::CreateConnection(database);
  auto collector = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(v INT);");
  ExecuteOne(*owner, "BEGIN;");
  ExecuteOne(*owner, "INSERT INTO t VALUES (1);");

  const auto stats = collector->GarbageCollect();
  EXPECT_EQ(stats.timed_out_, 1U);
  EXPECT_FALSE(owner->HasActiveTransaction());
  EXPECT_THROW(owner->RollbackTransaction(), ProgrammingException);
  EXPECT_EQ(ScalarInt(*setup, "SELECT COUNT(*) FROM t;"), 0);

  // This needs the exclusive schema lease that the timed-out owner used to retain indefinitely.
  static_cast<void>(ExecuteOne(*setup, "DROP TABLE t;"));
  EXPECT_EQ(database->GetCatalog().GetTable("t"), NULL_TABLE_INFO);
}

TEST(DatabaseConcurrencyTest, WorkerAndMemoryBudgetsAreGlobalAndReleaseOnEveryPath) {
  WorkerSlotManager workers(2);
  auto first = workers.Acquire();
  auto second = workers.Acquire();
  auto waiting = std::async(std::launch::async, [&] { return workers.Acquire(); });
  EXPECT_EQ(waiting.wait_for(1ms), std::future_status::timeout);
  EXPECT_EQ(workers.Used(), 2U);
  EXPECT_EQ(workers.Peak(), 2U);
  first = {};
  auto third = waiting.get();
  EXPECT_EQ(workers.Used(), 2U);
  second = {};
  third = {};
  EXPECT_EQ(workers.Used(), 0U);

  WorkerSlotManager parallel_workers(4);
  auto admission = parallel_workers.Acquire();
  auto additional = parallel_workers.TryAcquire(3);
  EXPECT_EQ(admission.Slots(), 1U);
  EXPECT_EQ(additional.Slots(), 3U);
  EXPECT_EQ(parallel_workers.Used(), 4U);
  EXPECT_FALSE(static_cast<bool>(parallel_workers.TryAcquire(1)));
  additional = {};
  admission = {};
  EXPECT_EQ(parallel_workers.Used(), 0U);

  GlobalQueryMemoryManager global(100);
  {
    QueryMemoryManager left;
    QueryMemoryManager right;
    left.SetBudget(100);
    right.SetBudget(100);
    left.SetGlobalManager(&global);
    right.SetGlobalManager(&global);
    EXPECT_TRUE(left.TryReserve(60));
    EXPECT_FALSE(right.TryReserve(50));
    EXPECT_TRUE(right.TryReserve(40));
    EXPECT_EQ(global.Used(), 100U);
    EXPECT_EQ(global.Peak(), 100U);
    right.Release(40);
    EXPECT_EQ(global.Used(), 60U);
    // left's outstanding reservation is deliberately released by its destructor (exception safety).
  }
  EXPECT_EQ(global.Used(), 0U);
}

TEST(DatabaseConcurrencyTest, DuplicateDurableOpenFailsThenCleanReopenSucceeds) {
  const auto path = TempDatabasePath("duplicate_open");
  std::filesystem::remove(path);
  auto first = std::make_shared<DatabaseInstance>(path);
  EXPECT_THROW(static_cast<void>(std::make_shared<DatabaseInstance>(path)), ExecutionException);
  first->Close();
  auto reopened = std::make_shared<DatabaseInstance>(path);
  reopened->Close();
  std::filesystem::remove(path);
}

TEST(DatabaseConcurrencyTest, DurableCloseWaitsForAdmittedWriteAndFlushesIt) {
  const auto path = TempDatabasePath("close_flush");
  std::filesystem::remove(path);
  DatabaseConfig config;
  config.worker_threads_ = 1;
  auto database = std::make_shared<DatabaseInstance>(path, config);
  auto connection = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*connection, "CREATE TABLE t(v INT);");

  auto occupied_slot = database->AcquireWorkerSlot();
  auto insert = std::async(std::launch::async, [&] { return ExecuteOne(*connection, "INSERT INTO t VALUES (7);"); });
  ASSERT_TRUE(SpinUntil([&] { return database->ActiveOperationCount() == 1; }));
  auto close = std::async(std::launch::async, [&] { database->Close(); });
  ASSERT_TRUE(SpinUntil([&] { return database->State() == DatabaseInstance::LifecycleState::CLOSING; }));
  occupied_slot = {};
  insert.get();
  close.get();

  auto reopened = std::make_shared<DatabaseInstance>(path, config);
  auto reader = DatabaseInstance::CreateConnection(reopened);
  EXPECT_EQ(ScalarInt(*reader, "SELECT SUM(v) FROM t;"), 7);
  reopened->Close();
  std::filesystem::remove(path);
}

}  // namespace bumblebee
