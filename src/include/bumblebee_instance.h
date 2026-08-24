//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumblebee_instance.h
//
// Identification: src/include/bumblebee_instance.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "main/connection.h"
#include "main/database_config.h"
#include "main/database_instance.h"
#include "main/query_result.h"
#include "main/result_writer.h"

namespace bumblebee {

/**
 * @brief Shell-facing compatibility adapter over one `DatabaseInstance` and named Connections.
 *
 * The database engine and transaction state live in the native owner/session classes. This adapter
 * exists only for the interactive shell and the legacy C++ frontend tests: `\session name` selects a
 * real independent `Connection`, while ordinary SQL follows the same path exposed to Python.
 */
class BumbleBeeInstance {
 public:
  /** @brief Open an in-memory database with default settings and the requested timeout. */
  explicit BumbleBeeInstance(duration_t txn_timeout = DEFAULT_TXN_TIMEOUT);
  /** @brief Open an in-memory database with a fully assembled immutable config. */
  explicit BumbleBeeInstance(DatabaseConfig config);
  /** @brief Open a durable database with legacy constructor arguments. */
  BumbleBeeInstance(const std::filesystem::path &db_file, size_t num_frames = BUFFER_POOL_SIZE,
                    duration_t txn_timeout = DEFAULT_TXN_TIMEOUT);
  /** @brief Open a durable database with a fully assembled immutable config. */
  BumbleBeeInstance(const std::filesystem::path &db_file, DatabaseConfig config);
  ~BumbleBeeInstance();

  BumbleBeeInstance(const BumbleBeeInstance &) = delete;
  auto operator=(const BumbleBeeInstance &) -> BumbleBeeInstance & = delete;

  /** @brief Execute SQL or a shell meta-command through the selected named Connection. */
  auto ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool;
  /** @brief Execute SQL through the selected named Connection and return detached results. */
  auto ExecuteSqlResults(const std::string &sql) -> std::vector<QueryResult>;
  /** @brief Seed demonstration tables through the default Connection. */
  void GenerateMockTable();
  /** @brief Create an independent native Connection for non-shell callers/tests. */
  auto Connect() -> std::shared_ptr<Connection> { return DatabaseInstance::CreateConnection(database_); }
  /** @brief Close every named session and the shared database. Idempotent. */
  void Close();

  /** @return The shared database owner. */
  auto GetDatabase() const -> const std::shared_ptr<DatabaseInstance> & { return database_; }
  /** @return The shared catalog for native test introspection. */
  auto GetCatalog() -> Catalog & { return database_->GetCatalog(); }

 private:
  auto CurrentConnection() -> Connection &;

  std::shared_ptr<DatabaseInstance> database_;
  std::unordered_map<std::string, std::shared_ptr<Connection>> sessions_;
  std::string current_session_{"default"};
  bool closed_{false};
};

}  // namespace bumblebee
