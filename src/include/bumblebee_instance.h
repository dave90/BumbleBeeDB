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
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog.h"
#include "concurrency/transaction_manager.h"
#include "database.h"
#include "execution/plans/abstract_plan.h"
#include "type/value.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"

namespace bumblebee {

class CreateStatement;
class DropStatement;
class TransactionStatement;
class ExplainStatement;
class BoundStatement;
class ClientContext;

/**
 * Where a statement's results are written. The instance drives this interface; the
 * shell, the tests and any future frontend each supply their own implementation.
 */
class ResultWriter {
 public:
  ResultWriter() = default;
  virtual ~ResultWriter() = default;

  virtual void WriteCell(const std::string &cell) = 0;
  virtual void WriteHeaderCell(const std::string &cell) = 0;
  virtual void BeginHeader() = 0;
  virtual void EndHeader() = 0;
  virtual void BeginRow() = 0;
  virtual void EndRow() = 0;
  virtual void BeginTable(bool simplified_output) = 0;
  virtual void EndTable() = 0;

  /** @brief Write a single value as a whole one-cell table. */
  virtual void OneCell(const std::string &cell) {
    BeginTable(true);
    BeginRow();
    WriteCell(cell);
    EndRow();
    EndTable();
  }

  /**
   * @brief The most result rows this writer wants emitted; 0 means unlimited.
   *
   * `WriteResultTable` stops after this many rows and then calls `WriteTruncationNotice`. The default
   * is unlimited so machine consumers (the tests' `StringVectorWriter`, the e2e protocol) see every row;
   * only the interactive shell's writer caps output.
   */
  virtual auto MaxDisplayRows() const -> idx_t { return 0; }

  /** @brief Called once when output was capped: `shown` of `total` rows were emitted. Default no-op. */
  virtual void WriteTruncationNotice(idx_t shown, idx_t total) {}

  bool simplified_output_{false};
};

/** @brief Discards everything. */
class NoopWriter : public ResultWriter {
 public:
  void WriteCell(const std::string &cell) override {}
  void WriteHeaderCell(const std::string &cell) override {}
  void BeginHeader() override {}
  void EndHeader() override {}
  void BeginRow() override {}
  void EndRow() override {}
  void BeginTable(bool simplified_output) override {}
  void EndTable() override {}
};

/** @brief Writes separator-delimited plain text to a stream. Used by the shell. */
class SimpleStreamWriter : public ResultWriter {
 public:
  explicit SimpleStreamWriter(std::ostream &stream, bool disable_header = false, std::string separator = "\t",
                              idx_t max_display_rows = 0)
      : disable_header_(disable_header),
        stream_(stream),
        separator_(std::move(separator)),
        max_display_rows_(max_display_rows) {}

  void WriteCell(const std::string &cell) override { stream_ << cell << separator_; }

  void WriteHeaderCell(const std::string &cell) override {
    if (!disable_header_) {
      stream_ << cell << separator_;
    }
  }

  void BeginHeader() override {}

  void EndHeader() override {
    if (!disable_header_) {
      stream_ << std::endl;
    }
  }

  void BeginRow() override {}
  void EndRow() override { stream_ << std::endl; }
  void BeginTable(bool simplified_output) override {}
  void EndTable() override {}

  auto MaxDisplayRows() const -> idx_t override { return max_display_rows_; }

  void WriteTruncationNotice(idx_t shown, idx_t total) override {
    stream_ << "-- showing first " << shown << " of " << total << " rows (--max-rows 0 to show all) --"
            << std::endl;
  }

  bool disable_header_;
  std::ostream &stream_;
  std::string separator_;
  /** Cap on rows emitted (0 = unlimited); the interactive shell sets this so a huge SELECT is truncated. */
  idx_t max_display_rows_;
};

/** @brief Collects the cells into a vector of rows. Used by the tests. */
class StringVectorWriter : public ResultWriter {
 public:
  void WriteCell(const std::string &cell) override { values_.back().push_back(cell); }
  void WriteHeaderCell(const std::string &cell) override {}
  void BeginHeader() override {}
  void EndHeader() override {}
  void BeginRow() override { values_.emplace_back(); }
  void EndRow() override {}
  void BeginTable(bool simplified_output) override { values_.clear(); }
  void EndTable() override {}

  std::vector<std::vector<std::string>> values_;
};

/**
 * A BumbleBeeDB database.
 *
 * This drives the whole SQL frontend: parse, bind, plan, optimize. There is no
 * execution engine yet, so a query is answered by printing the plan it would have
 * run; `CREATE TABLE` is the one statement that has a real effect, registering a
 * schema in the catalog.
 *
 * Two backing modes: the default is a purely in-memory catalog (no disk, no
 * persistence — used by the tests). The file constructor owns a durable `Database`
 * (disk + buffer pool + persistent catalog + transaction manager), so a table
 * created in one session is recovered on the next. Until the execution engine
 * exists, only `CREATE TABLE` produces durable state; DML still just prints a plan.
 */
class BumbleBeeInstance {
 public:
  /**
   * @brief An in-memory instance: no buffer pool, no disk, no persistence.
   * @param txn_timeout How long a transaction may stay open before `\gc` aborts it (tests lower it).
   */
  explicit BumbleBeeInstance(duration_t txn_timeout = DEFAULT_TXN_TIMEOUT);

  /**
   * @brief A durable instance backed by `db_file`: owns a `Database` and takes its catalog from there.
   * @param db_file     The database file to open (created if absent).
   * @param num_frames  Buffer-pool size in frames (defaults to the standard pool size).
   * @param txn_timeout How long a transaction may stay open before `\gc` aborts it (tests lower it).
   */
  explicit BumbleBeeInstance(const std::filesystem::path &db_file, size_t num_frames = BUFFER_POOL_SIZE,
                             duration_t txn_timeout = DEFAULT_TXN_TIMEOUT);

  ~BumbleBeeInstance();

  /**
   * @brief Parse, bind, plan and optimize a SQL string, writing the result to `writer`.
   *
   * @param sql One or more `;`-separated statements, or a `\`-prefixed meta-command.
   * @param writer Where the result goes.
   * @return bool True if every statement succeeded.
   */
  auto ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool;

  /** @brief Register a few tables so a fresh shell has something to query. */
  void GenerateMockTable();

  /** @return The owning Database in durable mode, or nullptr for an in-memory instance. */
  auto GetDatabase() -> Database * { return db_.get(); }

  /** The catalog (non-owning): the in-memory one, or the durable Database's. */
  Catalog *catalog_;
  /** The transaction manager (non-owning): the in-memory one, or the durable Database's. */
  TransactionManager *txn_mgr_;
  /** The buffer pool (non-owning): the in-memory one, or the durable Database's. */
  BufferPoolManager *bpm_;

  /** Force lowering to the out-of-core join/sort variants (tests toggle this to exercise the spill path). */
  bool prefer_external_{false};
  /** The per-query memory budget passed to each statement's ClientContext. */
  idx_t max_memory_{MAX_MEMORY};
  /** Worker-thread cap per statement; 0 leaves the ClientContext's hardware-detected default in place. */
  idx_t max_threads_{0};
  /** Heap pages per parallel-scan morsel passed to each statement's ClientContext. */
  idx_t morsel_pages_{MORSEL_PAGES};
  /** Hash-aggregate sink partitioning threshold override (0 = operator default; tests lower it). */
  idx_t agg_partition_threshold_{0};
  /** Target rows per columnar morsel passed to each statement's ClientContext (reserved). */
  idx_t morsel_size_{MORSEL_SIZE};

 private:
  void HandleCreateStatement(const CreateStatement &stmt, ResultWriter &writer);
  void HandleCreateExternalTable(const CreateStatement &stmt, ResultWriter &writer);
  void CmdVacuumExternal(const std::string &table_name, ResultWriter &writer);
  void HandleDropStatement(const DropStatement &stmt, ResultWriter &writer);
  void HandleTransactionStatement(const TransactionStatement &stmt, ResultWriter &writer);
  void HandleExplainStatement(const ExplainStatement &stmt, ResultWriter &writer);
  /** @brief Bind → plan → optimize → lower → execute one non-DDL statement, streaming rows to `writer`. */
  void ExecuteStatement(const BoundStatement &statement, ResultWriter &writer);
  /**
   * @brief Optimize + execute a planned uncorrelated scalar subquery to its single value.
   *
   * Runs inside the session's explicit transaction when one is open, else in its own autocommit
   * transaction. 0 rows -> NULL; more than 1 row -> ExecutionException.
   *
   * @param subplan The subquery's logical plan (one output column).
   * @return Value The single result cell.
   */
  auto EvalScalarSubquery(const AbstractPlanNodeRef &subplan) -> Value;
  /** @brief Copy this instance's tunables (memory, threads, morsel, spill) onto a statement's ClientContext. */
  void ApplyConfig(ClientContext &client) const;
  void CmdDisplayTables(ResultWriter &writer);
  void CmdDescribeTable(const std::string &table_name, ResultWriter &writer);
  void CmdDisplayHelp(ResultWriter &writer);
  void CmdClear(ResultWriter &writer);
  void CmdGarbageCollect(ResultWriter &writer);
  void WriteOneCell(const std::string &cell, ResultWriter &writer);

  /**
   * The current session's explicit transaction (opened by BEGIN), or null when its statements
   * autocommit. While set, every DML / SELECT statement runs inside it (seeing its own uncommitted
   * writes) until COMMIT or ROLLBACK; a statement that errors aborts and clears it. Non-owning — the
   * transaction manager owns the object.
   */
  auto ActiveTxn() -> Transaction *& { return session_txns_[current_session_]; }

  /**
   * Per-session explicit-transaction state. The shell is single-threaded, but `\session <name>`
   * switches which named session subsequent statements run in — each session can hold its own open
   * transaction, which is what makes deterministic concurrent-transaction tests (MVCC visibility,
   * write-write conflicts) possible through one shell process. Statements still execute one at a
   * time; only the transaction they join differs.
   */
  std::unordered_map<std::string, Transaction *> session_txns_;
  /** The session subsequent statements run in; `\session <name>` switches it (created on first use). */
  std::string current_session_{"default"};

  /** Durable backing (disk + BPM + catalog + txn manager); null for an in-memory instance. */
  std::unique_ptr<Database> db_;
  /** In-memory backing (used only when `db_` is null): a memory disk + buffer pool + catalog + txn mgr. */
  std::unique_ptr<MemoryDiskManager> mem_disk_;
  std::unique_ptr<BufferPoolManager> owned_bpm_;
  std::unique_ptr<Catalog> owned_catalog_;
  std::unique_ptr<TransactionManager> owned_txn_mgr_;
};

}  // namespace bumblebee
