//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database_instance_test.cpp
//
// Identification: test/unit/main/database_instance_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/database_instance.h"

#include <memory>
#include <string>
#include <utility>

#include "bumblebee_instance.h"
#include "common/exception.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/query_result.h"

namespace bumblebee {

static auto ExecuteOne(Connection &connection, const std::string &sql) -> QueryResult {
  auto results = connection.ExecuteSqlResults(sql);
  EXPECT_EQ(results.size(), 1U);
  return std::move(results.front());
}

static auto ScalarInt(Connection &connection, const std::string &sql) -> int64_t {
  auto result = ExecuteOne(connection, sql);
  const auto rows = result.MaterializeRows();
  EXPECT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().size(), 1U);
  return rows.front().front().GetAs<int64_t>();
}

TEST(DatabaseInstanceTest, ConnectionsShareCatalogAndAutocommitIndependently) {
  auto database = std::make_shared<DatabaseInstance>();
  auto first = DatabaseInstance::CreateConnection(database);
  auto second = DatabaseInstance::CreateConnection(database);

  ExecuteOne(*first, "CREATE TABLE t(v INT);");
  ExecuteOne(*first, "INSERT INTO t VALUES (1);");
  ExecuteOne(*second, "INSERT INTO t VALUES (2);");

  EXPECT_NE(database->GetCatalog().GetTable("t"), NULL_TABLE_INFO);
  EXPECT_EQ(ScalarInt(*first, "SELECT COUNT(*) FROM t;"), 2);
  EXPECT_EQ(ScalarInt(*second, "SELECT SUM(v) FROM t;"), 3);
}

TEST(DatabaseInstanceTest, ExplicitTransactionHasReadYourWritesAndSnapshotIsolation) {
  auto database = std::make_shared<DatabaseInstance>();
  auto writer = DatabaseInstance::CreateConnection(database);
  auto observer = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*writer, "CREATE TABLE t(v INT);");

  ExecuteOne(*writer, "BEGIN;");
  ExecuteOne(*writer, "INSERT INTO t VALUES (10);");
  EXPECT_TRUE(writer->HasActiveTransaction());
  EXPECT_EQ(ScalarInt(*writer, "SELECT COUNT(*) FROM t;"), 1);
  EXPECT_EQ(ScalarInt(*observer, "SELECT COUNT(*) FROM t;"), 0);
  ExecuteOne(*writer, "COMMIT;");
  EXPECT_FALSE(writer->HasActiveTransaction());
  EXPECT_EQ(ScalarInt(*observer, "SELECT COUNT(*) FROM t;"), 1);
}

TEST(DatabaseInstanceTest, ExplicitRollbackAndConnectionCloseRestoreCommittedState) {
  auto database = std::make_shared<DatabaseInstance>();
  auto setup = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*setup, "CREATE TABLE t(v INT);");
  ExecuteOne(*setup, "INSERT INTO t VALUES (1);");

  auto rollback = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*rollback, "BEGIN;");
  ExecuteOne(*rollback, "INSERT INTO t VALUES (2);");
  ExecuteOne(*rollback, "ROLLBACK;");
  EXPECT_EQ(ScalarInt(*setup, "SELECT SUM(v) FROM t;"), 1);

  auto abandoned = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*abandoned, "BEGIN;");
  ExecuteOne(*abandoned, "INSERT INTO t VALUES (3);");
  abandoned->Close();
  EXPECT_THROW(ExecuteOne(*abandoned, "SELECT 1;"), Exception);
  EXPECT_EQ(ScalarInt(*setup, "SELECT SUM(v) FROM t;"), 1);
}

TEST(DatabaseInstanceTest, ClosingOneConnectionDoesNotCloseDatabase) {
  auto database = std::make_shared<DatabaseInstance>();
  auto first = DatabaseInstance::CreateConnection(database);
  auto second = DatabaseInstance::CreateConnection(database);
  ExecuteOne(*first, "CREATE TABLE t(v INT);");
  first->Close();

  ExecuteOne(*second, "INSERT INTO t VALUES (9);");
  EXPECT_EQ(ScalarInt(*second, "SELECT v FROM t;"), 9);
}

TEST(DatabaseInstanceTest, ResultSurvivesLiteralConnectionDestruction) {
  auto database = std::make_shared<DatabaseInstance>();
  QueryResult result;
  {
    auto connection = DatabaseInstance::CreateConnection(database);
    ExecuteOne(*connection, "CREATE TABLE t(v VARCHAR);");
    ExecuteOne(*connection, "INSERT INTO t VALUES ('detached');");
    result = ExecuteOne(*connection, "SELECT v FROM t;");
  }

  ASSERT_EQ(result.MaterializeRows().size(), 1U);
  EXPECT_EQ(result.MaterializeRows()[0][0].GetString(), "detached");
}

TEST(DatabaseInstanceTest, ShellNamedSessionsAreNativeConnections) {
  BumbleBeeInstance shell;
  NoopWriter output;
  shell.ExecuteSql("CREATE TABLE t(v INT);", output);
  shell.ExecuteSql("\\session writer", output);
  shell.ExecuteSql("BEGIN;", output);
  shell.ExecuteSql("INSERT INTO t VALUES (5);", output);
  shell.ExecuteSql("\\session observer", output);

  StringVectorWriter rows;
  shell.ExecuteSql("SELECT COUNT(*) FROM t;", rows);
  ASSERT_EQ(rows.values_.size(), 1U);
  EXPECT_EQ(rows.values_[0][0], "0");

  shell.ExecuteSql("\\session writer", output);
  shell.ExecuteSql("COMMIT;", output);
  shell.ExecuteSql("\\session observer", output);
  shell.ExecuteSql("SELECT COUNT(*) FROM t;", rows);
  ASSERT_EQ(rows.values_.size(), 1U);
  EXPECT_EQ(rows.values_[0][0], "1");
}

TEST(DatabaseInstanceTest, DatabaseConfigurationIsSharedAndImmutableAtRuntime) {
  DatabaseConfig config;
  config.worker_threads_ = 3;
  config.max_memory_ = 123456;
  config.morsel_pages_ = 2;
  auto database = std::make_shared<DatabaseInstance>(config);
  auto first = DatabaseInstance::CreateConnection(database);
  auto second = DatabaseInstance::CreateConnection(database);

  EXPECT_EQ(first->GetDatabase()->Config().worker_threads_, 3U);
  EXPECT_EQ(second->GetDatabase()->Config().max_memory_, 123456U);
  EXPECT_EQ(&first->GetDatabase()->Config(), &second->GetDatabase()->Config());
}

}  // namespace bumblebee
