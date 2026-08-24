//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// connection_helpers_test.cpp
//
// Identification: test/unit/main/connection_helpers_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <chrono>  // NOLINT
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "common/exception.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database_instance.h"

namespace bumblebee {
namespace {

auto ScalarInt(Connection &connection, const std::string &sql) -> int64_t {
  const auto rows = connection.ExecuteSqlStatement(sql).MaterializeRows();
  EXPECT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().size(), 1U);
  return rows.front().front().GetAs<int64_t>();
}

auto TempDirectory() -> std::filesystem::path {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() / ("bumblebeedb_helpers_" + std::to_string(unique));
  std::filesystem::create_directories(path);
  return path;
}

auto QuoteSqlString(const std::filesystem::path &path) -> std::string {
  auto text = path.string();
  size_t offset = 0;
  while ((offset = text.find('\'', offset)) != std::string::npos) {
    text.insert(offset, 1, '\'');
    offset += 2;
  }
  return text;
}

}  // namespace

TEST(ConnectionHelpersTest, CatalogMetadataIsDetachedCompleteAndSorted) {
  auto database = std::make_shared<DatabaseInstance>();
  auto connection = DatabaseInstance::CreateConnection(database);
  connection->ExecuteSqlStatement("CREATE TABLE zed(value VARCHAR)");
  connection->ExecuteSqlStatement("CREATE TABLE accounts(id BIGINT PRIMARY KEY, active BOOLEAN)");
  connection->ExecuteSqlStatement("INSERT INTO accounts VALUES (1, NULL)");

  const auto tables = connection->ListTables();
  ASSERT_EQ(tables.size(), 2U);
  EXPECT_EQ(tables[0].name_, "accounts");
  EXPECT_EQ(tables[1].name_, "zed");

  const auto accounts = connection->DescribeTable("accounts");
  ASSERT_EQ(accounts.columns_.size(), 2U);
  EXPECT_EQ(accounts.columns_[0].name_, "id");
  EXPECT_EQ(accounts.columns_[0].type_, LogicalType(LogicalTypeId::BIGINT));
  EXPECT_TRUE(accounts.columns_[0].primary_key_);
  EXPECT_EQ(accounts.primary_key_, (std::vector<std::string>{"id"}));
  EXPECT_FALSE(accounts.generated_id_);
  EXPECT_EQ(accounts.storage_format_, StorageFormat::ROW);
  EXPECT_FALSE(accounts.location_.has_value());
  EXPECT_EQ(accounts.estimated_rows_, 1U);

  const auto generated = connection->DescribeTable("zed");
  EXPECT_TRUE(generated.generated_id_);
  EXPECT_EQ(generated.primary_key_, (std::vector<std::string>{"_id"}));
  EXPECT_THROW(static_cast<void>(connection->DescribeTable("missing")), BinderException);
}

TEST(ConnectionHelpersTest, ExplicitTransactionMethodsHonorIsolationAndMisuseRules) {
  auto database = std::make_shared<DatabaseInstance>();
  auto connection = DatabaseInstance::CreateConnection(database);
  connection->ExecuteSqlStatement("CREATE TABLE t(v INT)");

  connection->BeginTransaction(IsolationLevel::SERIALIZABLE);
  connection->ExecuteSqlStatement("INSERT INTO t VALUES (1)");
  connection->CommitTransaction();
  EXPECT_EQ(ScalarInt(*connection, "SELECT COUNT(*) FROM t"), 1);

  connection->BeginTransaction();
  connection->ExecuteSqlStatement("INSERT INTO t VALUES (2)");
  connection->RollbackTransaction();
  EXPECT_EQ(ScalarInt(*connection, "SELECT COUNT(*) FROM t"), 1);
  EXPECT_THROW(connection->CommitTransaction(), ProgrammingException);
  EXPECT_THROW(connection->RollbackTransaction(), ProgrammingException);
}

TEST(ConnectionHelpersTest, ScriptErrorsAreIndexedAndOpenTransactionIsRolledBack) {
  auto database = std::make_shared<DatabaseInstance>();
  auto connection = DatabaseInstance::CreateConnection(database);

  try {
    static_cast<void>(connection->ExecuteSqlScript(
        "CREATE TABLE t(v INT); INSERT INTO missing VALUES (1); INSERT INTO t VALUES (2)"));
    FAIL() << "script should have failed";
  } catch (const BinderException &) {
    FAIL() << "script context preserves the exception category through the base Exception type";
  } catch (const Exception &error) {
    EXPECT_EQ(error.GetType(), ExceptionType::BINDER);
    EXPECT_NE(std::string(error.what()).find("script statement 2"), std::string::npos);
  }
  EXPECT_EQ(ScalarInt(*connection, "SELECT COUNT(*) FROM t"), 0);

  try {
    static_cast<void>(connection->ExecuteSqlScript("SELECT 1; SELEC 2; SELECT 3"));
    FAIL() << "syntax error should have failed";
  } catch (const Exception &error) {
    EXPECT_EQ(error.GetType(), ExceptionType::PARSER);
    EXPECT_NE(std::string(error.what()).find("script statement 2"), std::string::npos);
  }

  EXPECT_THROW(static_cast<void>(connection->ExecuteSqlScript("BEGIN; INSERT INTO t VALUES (3)")),
               ProgrammingException);
  EXPECT_FALSE(connection->HasActiveTransaction());
  EXPECT_EQ(ScalarInt(*connection, "SELECT COUNT(*) FROM t"), 0);
}

TEST(ConnectionHelpersTest, VacuumExternalReturnsRemovedCountAndRejectsRows) {
  const auto directory = TempDirectory();
  auto database = std::make_shared<DatabaseInstance>();
  auto connection = DatabaseInstance::CreateConnection(database);
  connection->ExecuteSqlStatement("CREATE TABLE ext(v INT) WITH (format='parquet', location='" +
                                  QuoteSqlString(directory) + "')");
  connection->ExecuteSqlStatement("INSERT INTO ext VALUES (1), (2)");
  connection->ExecuteSqlStatement("INSERT INTO ext VALUES (3)");
  {
    std::ofstream orphan(directory / "orphan.parquet", std::ios::binary);
    orphan << "not live";
  }

  const auto metadata = connection->DescribeTable("ext");
  EXPECT_EQ(metadata.storage_format_, StorageFormat::PARQUET);
  EXPECT_EQ(metadata.location_, directory.string());
  EXPECT_EQ(metadata.estimated_rows_, 3U);
  EXPECT_GE(connection->VacuumTable("ext"), 1U);
  EXPECT_FALSE(std::filesystem::exists(directory / "orphan.parquet"));
  EXPECT_EQ(ScalarInt(*connection, "SELECT COUNT(*) FROM ext"), 3);

  connection->ExecuteSqlStatement("CREATE TABLE heap(v INT)");
  EXPECT_THROW(static_cast<void>(connection->VacuumTable("heap")), Exception);
  database->Close();
  std::filesystem::remove_all(directory);
}

}  // namespace bumblebee
