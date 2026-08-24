//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// connection.h
//
// Identification: src/include/main/connection.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <condition_variable>  // NOLINT
#include <exception>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "execution/plans/abstract_plan.h"
#include "concurrency/transaction.h"
#include "main/database_instance.h"
#include "main/query_result.h"
#include "main/result_writer.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

class BoundStatement;
class ClientContext;
class CreateStatement;
class DropStatement;
class ExplainStatement;
class TransactionStatement;

/** @brief One column in a detached catalog description returned by Connection helpers. */
struct ColumnMetadata {
  std::string name_;
  LogicalType type_;
  bool primary_key_{false};
};

/**
 * @brief A fully detached table description safe after catalog changes or database shutdown.
 *
 * Python and other frontends consume this value object instead of retaining pointers into Catalog.
 */
struct TableMetadata {
  std::string name_;
  std::vector<ColumnMetadata> columns_;
  std::vector<std::string> primary_key_;
  bool generated_id_{false};
  StorageFormat storage_format_{StorageFormat::ROW};
  std::optional<std::string> location_;
  idx_t estimated_rows_{0};
};

/**
 * @brief A sequential native database session owning at most one explicit transaction.
 *
 * Connections share all database facilities through `DatabaseInstance`. Closing one rolls back its
 * transaction without closing the database, and each connection accepts one statement at a time.
 */
class Connection {
 public:
  ~Connection();

  Connection(const Connection &) = delete;
  auto operator=(const Connection &) -> Connection & = delete;
  Connection(Connection &&) = delete;
  auto operator=(Connection &&) -> Connection & = delete;

  /** @brief Execute SQL and render each typed result for the shell/test adapter. */
  auto ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool;
  /** @brief Execute one or more SQL statements, returning one owning result per statement. */
  auto ExecuteSqlResults(const std::string &sql) -> std::vector<QueryResult>;
  /**
   * @brief Execute a script on this session, annotating failures with their one-based statement index.
   *
   * If a script opens a transaction that it does not close, the transaction is rolled back and the
   * call fails. A transaction already active before the call remains owned by the caller.
   */
  auto ExecuteSqlScript(const std::string &sql) -> std::vector<QueryResult>;
  /**
   * @brief Execute exactly one statement, rejecting scripts before any side effect occurs.
   * @param allow_transaction_control Whether BEGIN/COMMIT/ROLLBACK are valid for this call.
   */
  auto ExecuteSqlStatement(const std::string &sql, bool allow_transaction_control = true) -> QueryResult;
  /**
   * @brief Create or append a table from fully owned, user-width chunks.
   *
   * Chunks exclude the generated `_id` column. Schema validation happens before the first write and
   * all chunks are inserted in one native transaction through normal MVCC/index maintenance.
   */
  auto LoadDataChunks(std::string table_name, std::vector<std::string> column_names,
                      std::vector<LogicalType> column_types, std::vector<std::string> primary_key, bool append,
                      data_chunk_vector_t chunks) -> idx_t;
  /** @brief Begin an explicit transaction at the requested isolation level. */
  void BeginTransaction(IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION);
  /** @brief Commit this session's explicit transaction. */
  void CommitTransaction();
  /** @brief Roll back this session's explicit transaction. */
  void RollbackTransaction();
  /** @return Detached metadata for every table, sorted by name. */
  auto ListTables() -> std::vector<TableMetadata>;
  /** @return Detached metadata for one table, or throw a binder error when it is missing. */
  auto DescribeTable(const std::string &table_name) -> TableMetadata;
  /** @brief Vacuum an external parquet table and return the number of removed files. */
  auto VacuumTable(const std::string &table_name) -> size_t;
  /** @brief Run transaction timeout/reclamation GC and return its counters. */
  auto GarbageCollect() -> TransactionManager::GcStats;
  /** @brief Seed the shell's optional demonstration tables through the real SQL path. */
  void GenerateMockTable();
  /** @brief Roll back an open transaction and close this session. Idempotent. */
  void Close();

  /** @return The shared database owner. */
  auto GetDatabase() const -> const std::shared_ptr<DatabaseInstance> & { return database_; }
  /** @return Whether this session has been closed. */
  auto IsClosed() const -> bool { return closed_.load(std::memory_order_acquire); }
  /** @return Whether an explicit transaction is currently open. */
  auto HasActiveTransaction() const -> bool { return has_active_txn_.load(std::memory_order_acquire); }

 private:
  friend class DatabaseInstance;

  class StatementGuard {
   public:
    explicit StatementGuard(Connection &connection);
    ~StatementGuard();
    StatementGuard(const StatementGuard &) = delete;
    auto operator=(const StatementGuard &) -> StatementGuard & = delete;

   private:
    Connection *connection_;
  };

  explicit Connection(std::shared_ptr<DatabaseInstance> database);
  void RequestCloseFromDatabase();
  void CloseIdleFromDatabase();
  void CloseFromDatabase();
  void CloseInternal();
  void FinishCloseLocked();
  void ReconcileTransactionAfterGc();
  void ReconcileActiveTransactionLocked();
  auto ExecuteSqlResultsInternal(const std::string &sql, bool annotate_statement_errors = false)
      -> std::vector<QueryResult>;
  void AbortActiveTransaction();
  void BeginTransactionInternal(IsolationLevel isolation_level);
  void CommitTransactionInternal();
  void RollbackTransactionInternal();
  auto DescribeTableInternal(const std::shared_ptr<TableInfo> &table) const -> TableMetadata;
  auto VacuumTableInternal(const std::string &table_name) -> size_t;
  auto GarbageCollectInternal() -> TransactionManager::GcStats;
  auto AcquireQueryWorkers(ClientContext &client, idx_t demand) -> WorkerSlotManager::Token;

  auto HandleCreateStatement(const CreateStatement &stmt) -> QueryResult;
  auto HandleCreateExternalTable(const CreateStatement &stmt) -> QueryResult;
  auto HandleDropStatement(const DropStatement &stmt) -> QueryResult;
  auto HandleTransactionStatement(const TransactionStatement &stmt) -> QueryResult;
  auto HandleExplainStatement(const ExplainStatement &stmt) -> QueryResult;
  auto ExecuteStatement(const BoundStatement &statement) -> QueryResult;
  auto EvalScalarSubquery(const AbstractPlanNodeRef &subplan) -> Value;
  void ApplyConfig(ClientContext &client) const;

  void CmdDisplayTables(ResultWriter &writer);
  void CmdDescribeTable(const std::string &table_name, ResultWriter &writer);
  void CmdDisplayHelp(ResultWriter &writer);
  void CmdClear(ResultWriter &writer);
  void CmdGarbageCollect(ResultWriter &writer);
  void CmdVacuumExternal(const std::string &table_name, ResultWriter &writer);
  void WriteOneCell(const std::string &cell, ResultWriter &writer);

  auto ActiveTxn() -> std::shared_ptr<Transaction> & { return active_txn_; }
  auto ActiveTxnRaw() const -> Transaction * { return active_txn_.get(); }

  std::shared_ptr<DatabaseInstance> database_;
  Catalog *catalog_{nullptr};
  TransactionManager *txn_mgr_{nullptr};
  BufferPoolManager *bpm_{nullptr};
  std::shared_ptr<Transaction> active_txn_;
  std::optional<DatabaseInstance::SharedSchemaLease> explicit_schema_lease_;
  idx_t current_worker_slots_{1};

  mutable std::mutex state_mutex_;
  std::condition_variable state_cv_;
  bool busy_{false};
  bool closing_{false};
  bool reconcile_transaction_after_statement_{false};
  std::exception_ptr close_error_;
  std::atomic<bool> closed_{false};
  std::atomic<bool> has_active_txn_{false};
};

}  // namespace bumblebee
