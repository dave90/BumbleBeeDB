//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// connection.cpp
//
// Identification: src/main/connection.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/connection.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <filesystem>

#include "binder/binder.h"
#include "binder/bound_statement.h"
#include "binder/statement/create_statement.h"
#include "binder/statement/drop_statement.h"
#include "binder/statement/insert_statement.h"
#include "binder/statement/update_statement.h"
#include "binder/statement/delete_statement.h"
#include "binder/statement/explain_statement.h"
#include "binder/statement/transaction_statement.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/enums/statement_type.h"
#include "common/exception.h"
#include "common/util/string_util.h"
#include "execution/operator/helper/physical_result_collector.h"
#include "execution/operator/persistent/index_maintenance.h"
#include "execution/physical_plan_generator.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/limit_plan.h"
#include "fmt/format.h"
#include "main/client_context.h"
#include "nodes/nodes.hpp"
#include "nodes/parsenodes.hpp"
#include "optimizer/optimizer.h"
#include "parallel/executor.h"
#include "storage/parquet/external_schema.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/parquet/parquet_reader.h"
#include "storage/parquet/parquet_table_ops.h"
#include "planner/planner.h"
#include "storage/table/parquet_table.h"
#include "type/logical_type.h"

namespace bumblebee {

namespace {

/**
 * @brief Estimate the one-based SQL statement containing a parser error byte offset.
 *
 * libpg_query reports a source offset even when it cannot produce a parse tree. This small lexer
 * counts only top-level semicolons, ignoring quoted strings/identifiers, dollar strings, and
 * comments, so script diagnostics remain useful on malformed input.
 */
auto ScriptStatementIndex(const std::string &sql, int error_location) -> size_t {
  const auto limit = error_location < 0 ? sql.size() : std::min(sql.size(), static_cast<size_t>(error_location));
  size_t statement = 1;
  bool has_content = false;
  size_t block_comment_depth = 0;
  enum class QuoteState : uint8_t { NONE, SINGLE, DOUBLE, LINE_COMMENT, DOLLAR };
  QuoteState state = QuoteState::NONE;
  std::string dollar_delimiter;

  for (size_t i = 0; i < limit; i++) {
    const auto current = sql[i];
    const auto next = i + 1 < limit ? sql[i + 1] : '\0';

    if (block_comment_depth > 0) {
      if (current == '/' && next == '*') {
        block_comment_depth++;
        i++;
      } else if (current == '*' && next == '/') {
        block_comment_depth--;
        i++;
      }
      continue;
    }
    if (state == QuoteState::LINE_COMMENT) {
      if (current == '\n' || current == '\r') {
        state = QuoteState::NONE;
      }
      continue;
    }
    if (state == QuoteState::SINGLE) {
      if (current == '\\' && i + 1 < limit) {
        i++;
      } else if (current == '\'' && next == '\'') {
        i++;
      } else if (current == '\'') {
        state = QuoteState::NONE;
      }
      continue;
    }
    if (state == QuoteState::DOUBLE) {
      if (current == '"' && next == '"') {
        i++;
      } else if (current == '"') {
        state = QuoteState::NONE;
      }
      continue;
    }
    if (state == QuoteState::DOLLAR) {
      if (sql.compare(i, dollar_delimiter.size(), dollar_delimiter) == 0) {
        i += dollar_delimiter.size() - 1;
        state = QuoteState::NONE;
      }
      continue;
    }

    if (current == '-' && next == '-') {
      state = QuoteState::LINE_COMMENT;
      i++;
      continue;
    }
    if (current == '/' && next == '*') {
      block_comment_depth = 1;
      i++;
      continue;
    }
    if (current == '\'') {
      state = QuoteState::SINGLE;
      has_content = true;
      continue;
    }
    if (current == '"') {
      state = QuoteState::DOUBLE;
      has_content = true;
      continue;
    }
    if (current == '$') {
      auto end = i + 1;
      while (end < limit && (std::isalnum(static_cast<unsigned char>(sql[end])) != 0 || sql[end] == '_')) {
        end++;
      }
      if (end < limit && sql[end] == '$') {
        dollar_delimiter = sql.substr(i, end - i + 1);
        state = QuoteState::DOLLAR;
        has_content = true;
        i = end;
        continue;
      }
    }
    if (current == ';') {
      if (has_content) {
        statement++;
        has_content = false;
      }
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(current)) == 0) {
      has_content = true;
    }
  }
  return statement;
}

void ValidateTableHelperName(const std::string &name) {
  if (name.empty() || name.find('\0') != std::string::npos) {
    throw ProgrammingException("table name must be a non-empty string without NUL bytes");
  }
}

/** @brief Classify a raw parser node before binding touches catalog-owned objects. */
auto RawStatementType(duckdb_libpgquery::PGNode *node) -> StatementType {
  while (node != nullptr && node->type == duckdb_libpgquery::T_PGRawStmt) {
    node = reinterpret_cast<duckdb_libpgquery::PGRawStmt *>(node)->stmt;
  }
  if (node == nullptr) {
    return StatementType::INVALID_STATEMENT;
  }
  switch (node->type) {
    case duckdb_libpgquery::T_PGCreateStmt:
      return StatementType::CREATE_STATEMENT;
    case duckdb_libpgquery::T_PGDropStmt:
      return StatementType::DROP_STATEMENT;
    case duckdb_libpgquery::T_PGTransactionStmt:
      return StatementType::TRANSACTION_STATEMENT;
    case duckdb_libpgquery::T_PGInsertStmt:
      return StatementType::INSERT_STATEMENT;
    case duckdb_libpgquery::T_PGSelectStmt:
      return StatementType::SELECT_STATEMENT;
    case duckdb_libpgquery::T_PGExplainStmt:
      return StatementType::EXPLAIN_STATEMENT;
    case duckdb_libpgquery::T_PGDeleteStmt:
      return StatementType::DELETE_STATEMENT;
    case duckdb_libpgquery::T_PGUpdateStmt:
      return StatementType::UPDATE_STATEMENT;
    default:
      return StatementType::INVALID_STATEMENT;
  }
}

/** @brief Pins a transaction against timeout rollback for one executing statement. */
class TransactionActivityGuard {
 public:
  explicit TransactionActivityGuard(std::shared_ptr<Transaction> transaction) : transaction_(std::move(transaction)) {
    if (transaction_ != nullptr) {
      transaction_->EnterStatement();
    }
  }
  ~TransactionActivityGuard() {
    if (transaction_ != nullptr) {
      transaction_->LeaveStatement();
    }
  }

  TransactionActivityGuard(const TransactionActivityGuard &) = delete;
  auto operator=(const TransactionActivityGuard &) -> TransactionActivityGuard & = delete;

 private:
  std::shared_ptr<Transaction> transaction_;
};

}  // namespace

Connection::Connection(std::shared_ptr<DatabaseInstance> database) : database_(std::move(database)) {
  if (database_ == nullptr) {
    throw Exception("a Connection requires a DatabaseInstance");
  }
  catalog_ = &database_->GetCatalog();
  bpm_ = &database_->GetBufferPool();
  txn_mgr_ = &database_->GetTransactionManager();
}

Connection::~Connection() {
  try {
    CloseInternal();
  } catch (...) {
    // An explicit Close() reports rollback failures. Destruction cannot throw.
  }
}

Connection::StatementGuard::StatementGuard(Connection &connection) : connection_(&connection) {
  std::lock_guard lock(connection_->state_mutex_);
  if (connection_->closed_.load(std::memory_order_acquire) || connection_->closing_) {
    throw ProgrammingException("connection is closed");
  }
  if (connection_->busy_) {
    throw ProgrammingException("concurrent statement calls on one Connection are not allowed");
  }
  connection_->busy_ = true;
}

Connection::StatementGuard::~StatementGuard() {
  std::lock_guard lock(connection_->state_mutex_);
  connection_->busy_ = false;
  if (connection_->closing_) {
    // Database shutdown marks every connection before it waits for admitted operations. Finishing
    // the close here releases an explicit transaction's long-lived schema lease before this public
    // operation drops its database operation token.
    connection_->FinishCloseLocked();
  } else if (connection_->reconcile_transaction_after_statement_) {
    connection_->ReconcileActiveTransactionLocked();
  }
  connection_->state_cv_.notify_all();
}

void Connection::AbortActiveTransaction() {
  if (active_txn_ != nullptr) {
    const auto state = active_txn_->GetTransactionState();
    if (state == TransactionState::RUNNING || state == TransactionState::TAINTED) {
      txn_mgr_->Abort(active_txn_.get());
    }
    active_txn_.reset();
    has_active_txn_.store(false, std::memory_order_release);
  }
  explicit_schema_lease_.reset();
}

void Connection::FinishCloseLocked() {
  if (closed_.load(std::memory_order_acquire)) {
    return;
  }
  try {
    AbortActiveTransaction();
  } catch (...) {
    if (close_error_ == nullptr) {
      close_error_ = std::current_exception();
    }
    active_txn_.reset();
    has_active_txn_.store(false, std::memory_order_release);
    explicit_schema_lease_.reset();
  }
  reconcile_transaction_after_statement_ = false;
  closed_.store(true, std::memory_order_release);
  closing_ = false;
  state_cv_.notify_all();
}

void Connection::CloseInternal() {
  std::exception_ptr error;
  {
    std::unique_lock lock(state_mutex_);
    if (!closed_.load(std::memory_order_acquire)) {
      closing_ = true;
      if (busy_) {
        state_cv_.wait(lock, [&] { return closed_.load(std::memory_order_acquire); });
      } else {
        FinishCloseLocked();
      }
    }
    error = close_error_;
    close_error_ = nullptr;
  }
  if (error != nullptr) {
    std::rethrow_exception(error);
  }
}

void Connection::Close() { CloseInternal(); }

void Connection::RequestCloseFromDatabase() {
  std::lock_guard lock(state_mutex_);
  if (!closed_.load(std::memory_order_acquire)) {
    closing_ = true;
  }
}

void Connection::CloseIdleFromDatabase() {
  std::lock_guard lock(state_mutex_);
  if (!closed_.load(std::memory_order_acquire) && closing_ && !busy_) {
    FinishCloseLocked();
  }
}

void Connection::CloseFromDatabase() { CloseInternal(); }

void Connection::ReconcileActiveTransactionLocked() {
  reconcile_transaction_after_statement_ = false;
  if (active_txn_ == nullptr) {
    return;
  }
  const auto state = active_txn_->GetTransactionState();
  const bool cancelled = active_txn_->IsCancellationRequested();
  if ((state == TransactionState::RUNNING || state == TransactionState::TAINTED) && !cancelled) {
    return;
  }
  try {
    AbortActiveTransaction();
  } catch (...) {
    // GC already made this transaction unusable. Keep the Connection and its schema lease coherent
    // even if rollback reported an error; the originating GC call has already surfaced rollback
    // failures from TransactionManager::GarbageCollection itself.
    active_txn_.reset();
    has_active_txn_.store(false, std::memory_order_release);
    explicit_schema_lease_.reset();
  }
}

void Connection::ReconcileTransactionAfterGc() {
  std::lock_guard lock(state_mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return;
  }
  if (busy_) {
    reconcile_transaction_after_statement_ = true;
    return;
  }
  ReconcileActiveTransactionLocked();
}

void Connection::WriteOneCell(const std::string &cell, ResultWriter &writer) { writer.OneCell(cell); }

void Connection::GenerateMockTable() {
  // Seed a few demo tables *with rows* (via the real SQL path) so a fresh shell has data to query out of
  // the box — two integer tables that join on their first column, plus one with a string column.
  NoopWriter noop;
  ExecuteSql("CREATE TABLE mock_ints_1(colA INT, colB INT);", noop);
  ExecuteSql("INSERT INTO mock_ints_1 VALUES (1, 100), (2, 200), (3, 300), (4, 400);", noop);
  ExecuteSql("CREATE TABLE mock_ints_2(colC INT, colD INT);", noop);
  ExecuteSql("INSERT INTO mock_ints_2 VALUES (1, 10), (2, 20), (3, 30);", noop);
  ExecuteSql("CREATE TABLE mock_people(id INT, name VARCHAR(32), age INT);", noop);
  ExecuteSql("INSERT INTO mock_people VALUES (1, 'alice', 30), (2, 'bob', 25), (3, 'carol', 41);", noop);
}

void Connection::CmdDisplayTables(ResultWriter &writer) {
  auto table_names = catalog_->GetTableNames();
  std::sort(table_names.begin(), table_names.end());

  writer.BeginTable(false);
  writer.BeginHeader();
  writer.WriteHeaderCell("oid");
  writer.WriteHeaderCell("name");
  writer.WriteHeaderCell("cols");
  writer.EndHeader();
  for (const auto &name : table_names) {
    const auto table_info = catalog_->GetTable(name);
    writer.BeginRow();
    writer.WriteCell(fmt::format("{}", table_info->oid_));
    writer.WriteCell(table_info->name_);
    writer.WriteCell(table_info->schema_.ToString());
    writer.EndRow();
  }
  writer.EndTable();
}

void Connection::CmdDescribeTable(const std::string &table_name, ResultWriter &writer) {
  const auto table_info = catalog_->GetTable(table_name);
  if (table_info == NULL_TABLE_INFO) {
    throw Exception(fmt::format("no such table: {}", table_name));
  }
  const auto &schema = table_info->schema_;

  // One row per column: ordinal, name, type, and on-page storage width in bytes.
  writer.BeginTable(false);
  writer.BeginHeader();
  writer.WriteHeaderCell("#");
  writer.WriteHeaderCell("column");
  writer.WriteHeaderCell("type");
  writer.WriteHeaderCell("bytes");
  writer.EndHeader();
  for (uint32_t i = 0; i < schema.GetColumnCount(); i++) {
    const auto &col = schema.GetColumn(i);
    writer.BeginRow();
    writer.WriteCell(fmt::format("{}", i));
    writer.WriteCell(col.GetName());
    writer.WriteCell(col.GetType().ToString());
    writer.WriteCell(fmt::format("{}", col.GetStorageSize()));
    writer.EndRow();
  }
  writer.EndTable();

  // Any indexes on the table, listed after the columns (mirrors psql's `\d`).
  std::vector<std::shared_ptr<IndexInfo>> table_indexes;
  for (const auto &idx : catalog_->GetIndexes()) {
    if (idx->table_name_ == table_name) {
      table_indexes.push_back(idx);
    }
  }
  if (!table_indexes.empty()) {
    writer.BeginTable(false);
    writer.BeginHeader();
    writer.WriteHeaderCell("index");
    writer.WriteHeaderCell("key columns");
    writer.EndHeader();
    for (const auto &idx : table_indexes) {
      std::string key_cols;
      for (const auto &col : idx->key_schema_.GetColumns()) {
        if (!key_cols.empty()) {
          key_cols += ", ";
        }
        key_cols += col.GetName();
      }
      writer.BeginRow();
      writer.WriteCell(idx->name_);
      writer.WriteCell(fmt::format("({})", key_cols));
      writer.EndRow();
    }
    writer.EndTable();
  }
}

void Connection::CmdClear(ResultWriter &writer) {
  const auto dropped = catalog_->DropAllTables();
  WriteOneCell(fmt::format("Cleared the database: dropped {} table(s).", dropped), writer);
}

void Connection::CmdGarbageCollect(ResultWriter &writer) {
  const auto stats = GarbageCollectInternal();

  // Only the timeout-abort count is reported: it is what the caller (and the timeout tests) can
  // predict, while the reclaimed count depends on the whole history of finished transactions.
  WriteOneCell(fmt::format("GC: aborted {} timed-out transaction(s)", stats.timed_out_), writer);
}

auto Connection::GarbageCollectInternal() -> TransactionManager::GcStats {
  return database_->GarbageCollectTransactions();
}

void Connection::CmdDisplayHelp(ResultWriter &writer) {
  std::string help = R"(Welcome to the BumbleBeeDB shell!

Meta-commands:
  \dt              show all tables
  \d <table>       describe a table's columns and indexes
  \clear           drop every table (wipe the database)
  \pipelines <sql> show the pipeline plan for a query
  \session <name>  switch to the named session (created on first use); each
                   session can hold its own open transaction
  \gc              run transaction garbage collection: reclaims old versions and
                   aborts transactions open longer than the timeout (--txn-timeout)
  \help            show this message again

BumbleBeeDB runs SQL end to end: CREATE TABLE / DROP TABLE / INSERT / SELECT
(with joins, aggregation, ORDER BY, LIMIT), plus UPDATE and DELETE. Statements
autocommit by default; wrap several in BEGIN ... COMMIT (or ROLLBACK to discard)
to run them in one explicit transaction. Prefix any query with EXPLAIN (e.g.
EXPLAIN (optimizer) SELECT ...) to see the binder, planner, optimized, and
physical plans instead of running it.
)";
  WriteOneCell(help, writer);
}

auto Connection::HandleCreateStatement(const CreateStatement &stmt) -> QueryResult {
  if (stmt.format_ == StorageFormat::PARQUET) {
    return HandleCreateExternalTable(stmt);
  }
  Schema schema(stmt.columns_);

  // Resolve primary-key column names (the binder guaranteed they exist) to their indices.
  std::vector<uint32_t> pk_attrs;
  pk_attrs.reserve(stmt.primary_key_.size());
  for (const auto &pk_name : stmt.primary_key_) {
    for (uint32_t i = 0; i < schema.GetColumnCount(); i++) {
      if (schema.GetColumn(i).GetName() == pk_name) {
        pk_attrs.push_back(i);
        break;
      }
    }
  }
  // The primary key is auto-generated iff column 0 is the reserved `_id` (the binder prepends it).
  const bool auto_id = schema.GetColumnCount() > 0 && schema.GetColumn(0).GetName() == AUTO_ID_COLUMN;

  auto info = catalog_->CreateTable(stmt.table_, schema, StorageFormat::ROW, pk_attrs, auto_id);
  if (info == NULL_TABLE_INFO) {
    throw Exception(fmt::format("failed to create table {}: it already exists", stmt.table_));
  }
  // Every table has a primary key, so build its B+tree index (needs a storage-backed catalog).
  if (!pk_attrs.empty() && info->storage_ != nullptr) {
    catalog_->CreateIndexForKey("_pk_" + stmt.table_, stmt.table_, pk_attrs);
  }
  return QueryResult::Command("CREATE TABLE", fmt::format("Table created with id = {}", info->oid_), std::nullopt,
                              StatementType::CREATE_STATEMENT);
}

auto Connection::HandleCreateExternalTable(const CreateStatement &stmt) -> QueryResult {
  namespace fs = std::filesystem;
  const auto &location = stmt.location_;

  // The folder is created if missing (a brand-new empty external table).
  std::error_code ec;
  fs::create_directories(location, ec);
  if (ec) {
    throw Exception(fmt::format("cannot create external table location '{}': {}", location, ec.message()));
  }

  // Live files: the newest manifest when one exists, otherwise every *.parquet in the folder
  // (adoption of a foreign directory).
  auto existing_manifest = ParquetManifestIO::ReadLatest(location);
  std::vector<ManifestEntry> entries;
  if (existing_manifest.has_value()) {
    entries = existing_manifest->entries_;
  } else {
    for (const auto &file : ParquetManifestIO::ListParquetFiles(location)) {
      entries.push_back(ManifestEntry{file, 0});  // row counts filled from footers below
    }
  }

  auto &allocator = GlobalParquetAllocator();
  std::optional<Schema> schema;
  if (!stmt.columns_.empty()) {
    schema = Schema(stmt.columns_);
  } else if (entries.empty()) {
    throw Exception(fmt::format(
        "cannot infer schema for external table '{}': location '{}' has no parquet files; declare the columns",
        stmt.table_, location));
  }

  // Validate every live file against the schema (declared or inferred from the first file), and
  // collect row counts for the manifest.
  for (auto &entry : entries) {
    auto file_path = (fs::path(location) / entry.file_name_).string();
    ParquetReader reader(allocator, file_path);
    if (!schema.has_value()) {
      // Inference: the first file defines the schema.
      std::vector<Column> columns;
      columns.reserve(reader.names_.size());
      for (idx_t i = 0; i < reader.names_.size(); i++) {
        columns.push_back(Column::Make(reader.names_[i], reader.return_types_[i]));
      }
      schema = Schema(columns);
    } else if (!ExternalSchemaMatches(*schema, reader.names_, reader.return_types_)) {
      throw Exception(fmt::format("external table '{}': parquet file '{}' does not match the {} schema", stmt.table_,
                                  entry.file_name_, stmt.columns_.empty() ? "inferred" : "declared"));
    }
    entry.row_count_ = reader.NumRows();
  }

  // Materialize the manifest if the folder had none: adopted files (or an empty list) become
  // version 0. From here on, directory contents no longer matter — only the manifest does.
  if (!existing_manifest.has_value()) {
    ParquetManifest manifest;
    manifest.version_ = 0;
    manifest.entries_ = entries;
    ParquetManifestIO::Write(location, manifest);
  }

  auto info = catalog_->CreateTable(stmt.table_, *schema, StorageFormat::PARQUET, {}, false, location);
  if (info == NULL_TABLE_INFO) {
    throw Exception(fmt::format("failed to create table {}: it already exists", stmt.table_));
  }
  return QueryResult::Command("CREATE TABLE",
                              fmt::format("External table created with id = {} at '{}'", info->oid_, location),
                              std::nullopt, StatementType::CREATE_STATEMENT);
}

void Connection::CmdVacuumExternal(const std::string &table_name, ResultWriter &writer) {
  auto info = catalog_->GetTable(table_name);
  auto *parquet = info == nullptr ? nullptr : dynamic_cast<ParquetTable *>(info->storage_.get());
  const auto path = parquet == nullptr ? std::string{} : parquet->GetPath();
  const auto removed = VacuumTableInternal(table_name);
  WriteOneCell(fmt::format("Vacuumed {} file(s) from '{}'", removed, path), writer);
}

auto Connection::VacuumTableInternal(const std::string &table_name) -> size_t {
  namespace fs = std::filesystem;
  auto info = catalog_->GetTable(table_name);
  if (info == NULL_TABLE_INFO) {
    throw Exception(fmt::format("no such table: {}", table_name));
  }
  auto *parquet = dynamic_cast<ParquetTable *>(info->storage_.get());
  if (parquet == nullptr) {
    throw Exception(fmt::format("vacuum only applies to external parquet tables ('{}' is row-format)", table_name));
  }
  const auto &dir = parquet->GetPath();

  // The writer lock keeps the sweep from racing an in-flight rewrite (whose fresh part files are
  // not yet referenced by any manifest and would look like orphans).
  ExternalWriteGuard guard(*parquet, table_name);
  auto manifest = ParquetManifestIO::ReadLatest(dir);
  if (!manifest.has_value()) {
    throw Exception(fmt::format("external table '{}': no manifest found at '{}'", table_name, dir));
  }
  std::unordered_set<std::string> live;
  for (const auto &e : manifest->entries_) {
    live.insert(e.file_name_);
  }
  const auto newest_manifest = fmt::format("{}{}", ParquetManifestIO::MANIFEST_PREFIX, manifest->version_);

  size_t removed = 0;
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto name = entry.path().filename().string();
    const bool orphan_part = name.ends_with(".parquet") && !live.contains(name);
    const bool old_manifest = name.starts_with(ParquetManifestIO::MANIFEST_PREFIX) && name != newest_manifest;
    if (orphan_part || old_manifest) {
      std::error_code ec;
      if (fs::remove(entry.path(), ec)) {
        removed++;
      }
    }
  }
  return removed;
}

auto Connection::HandleDropStatement(const DropStatement &stmt) -> QueryResult {
  size_t dropped = 0;
  for (const auto &table : stmt.tables_) {
    // DropTable removes the table and every index defined on it (including the auto primary-key index).
    if (catalog_->DropTable(table)) {
      dropped++;
    } else if (!stmt.if_exists_) {
      // Without IF EXISTS, a missing table is an error. Any tables named earlier in the same statement
      // have already been dropped — DROP is not transactional here.
      throw Exception(fmt::format("cannot drop table {}: it does not exist", table));
    }
  }
  return QueryResult::Command("DROP TABLE", fmt::format("Dropped {} table(s)", dropped), static_cast<int64_t>(dropped),
                              StatementType::DROP_STATEMENT);
}

auto Connection::HandleTransactionStatement(const TransactionStatement &stmt) -> QueryResult {
  switch (stmt.txn_type_) {
    case TransactionType::BEGIN:
      BeginTransactionInternal(IsolationLevel::SNAPSHOT_ISOLATION);
      break;
    case TransactionType::COMMIT:
      CommitTransactionInternal();
      break;
    case TransactionType::ROLLBACK:
      RollbackTransactionInternal();
      break;
  }
  const auto tag = TransactionTypeToString(stmt.txn_type_);
  return QueryResult::Command(tag, tag, std::nullopt, StatementType::TRANSACTION_STATEMENT);
}

void Connection::BeginTransactionInternal(IsolationLevel isolation_level) {
  auto &active_txn = ActiveTxn();
  if (active_txn != nullptr) {
    throw ProgrammingException("cannot BEGIN: a transaction is already in progress");
  }
  explicit_schema_lease_.emplace(database_->AcquireSharedSchemaLease());
  try {
    active_txn = txn_mgr_->BeginShared(isolation_level);
    has_active_txn_.store(true, std::memory_order_release);
  } catch (...) {
    explicit_schema_lease_.reset();
    throw;
  }
}

void Connection::CommitTransactionInternal() {
  auto &active_txn = ActiveTxn();
  if (active_txn == nullptr) {
    throw ProgrammingException("cannot COMMIT: no transaction is in progress");
  }
  bool committed;
  try {
    committed = txn_mgr_->Commit(active_txn.get());
  } catch (...) {
    const auto state = active_txn->GetTransactionState();
    if (state == TransactionState::RUNNING || state == TransactionState::TAINTED) {
      txn_mgr_->Abort(active_txn.get());
    }
    active_txn.reset();
    has_active_txn_.store(false, std::memory_order_release);
    explicit_schema_lease_.reset();
    throw;
  }
  active_txn.reset();
  has_active_txn_.store(false, std::memory_order_release);
  explicit_schema_lease_.reset();
  if (!committed) {
    throw ConflictException("cannot COMMIT: serializable validation failed, transaction aborted");
  }
}

void Connection::RollbackTransactionInternal() {
  auto &active_txn = ActiveTxn();
  if (active_txn == nullptr) {
    throw ProgrammingException("cannot ROLLBACK: no transaction is in progress");
  }
  txn_mgr_->Abort(active_txn.get());
  active_txn.reset();
  has_active_txn_.store(false, std::memory_order_release);
  explicit_schema_lease_.reset();
}

auto Connection::HandleExplainStatement(const ExplainStatement &stmt) -> QueryResult {
  std::string output;

  if ((stmt.options_ & ExplainOptions::BINDER) != 0) {
    output += "=== BINDER ===\n";
    output += stmt.statement_->ToString();
    output += "\n";
  }

  Planner planner(*catalog_);
  // EXPLAIN ANALYZE actually runs the query, so its scalar subqueries must pre-execute like a real
  // run. Every other EXPLAIN variant must not execute anything: no eval hook, and the planner keeps
  // each subquery behind a "(subquery)" placeholder instead.
  if ((stmt.options_ & ExplainOptions::ANALYZE) != 0) {
    planner.subquery_eval_ = [this](const AbstractPlanNodeRef &subplan) { return EvalScalarSubquery(subplan); };
  }
  planner.PlanQuery(*stmt.statement_);

  const bool show_schema = (stmt.options_ & ExplainOptions::SCHEMA) != 0;

  if ((stmt.options_ & ExplainOptions::PLANNER) != 0) {
    output += "=== PLANNER ===\n";
    output += planner.plan_->ToString(show_schema);
    output += "\n";
  }

  Optimizer optimizer(*catalog_);
  auto optimized_plan = optimizer.Optimize(planner.plan_);

  if ((stmt.options_ & ExplainOptions::OPTIMIZER) != 0) {
    output += "=== OPTIMIZER ===\n";
    output += optimized_plan->ToString(show_schema);
    output += "\n";
  }

  const bool want_physical = (stmt.options_ & ExplainOptions::PHYSICAL) != 0;
  const bool want_pipelines = (stmt.options_ & ExplainOptions::PIPELINES) != 0;
  const bool want_analyze = (stmt.options_ & ExplainOptions::ANALYZE) != 0;

  if (want_physical || want_pipelines || want_analyze) {
    ClientContext client(*catalog_, *txn_mgr_, bpm_);
    ApplyConfig(client);
    // Run inside the explicit transaction if one is open (so EXPLAIN ANALYZE sees its writes and does not
    // commit it early); otherwise autocommit a fresh transaction for the analyze run.
    const bool autocommit = ActiveTxn() == nullptr;
    auto txn = autocommit ? txn_mgr_->BeginShared() : ActiveTxn();
    TransactionActivityGuard activity(txn);
    client.txn_ = txn.get();
    try {
      PhysicalPlanGenerator generator(client);
      auto physical = generator.PlanRoot(optimized_plan);

      if (want_physical) {
        output += "=== PHYSICAL ===\n" + physical->ToString() + "\n";
      }
      if (want_pipelines || want_analyze) {
        Executor executor(client);
        executor.Initialize(*physical);
        std::optional<WorkerSlotManager::Token> query_workers;
        if (want_analyze) {
          query_workers.emplace(AcquireQueryWorkers(client, executor.PeakTaskDemand()));
          executor.ExecuteQuery();  // EXPLAIN ANALYZE actually runs the query
          output += "=== ANALYZE ===\n" + executor.AnalyzeToString(*physical) + "\n";
        }
        if (want_pipelines) {
          output += "=== PIPELINES ===\n" + executor.PipelinesToString() + "\n";
        }
      }
      if (autocommit) {
        txn->ThrowIfCancellationRequested();
        txn_mgr_->Commit(txn.get());
      }
    } catch (...) {
      if (autocommit) {
        txn_mgr_->Abort(txn.get());
      } else {
        AbortActiveTransaction();  // a failed statement aborts the explicit transaction
      }
      throw;
    }
  }

  return QueryResult::Command("EXPLAIN", std::move(output), std::nullopt, StatementType::EXPLAIN_STATEMENT);
}

void Connection::ApplyConfig(ClientContext &client) const {
  const auto &config = database_->Config();
  client.config_.prefer_external_ = config.prefer_external_;
  client.config_.max_memory_ = config.max_memory_;
  client.config_.morsel_pages_ = std::max<idx_t>(1, config.morsel_pages_);
  client.config_.morsel_size_ = config.morsel_size_;
  client.config_.agg_partition_threshold_ = config.aggregate_partition_threshold_;
  // Planning sees the database ceiling. Immediately before execution AcquireQueryWorkers narrows it
  // to the slots this query actually owns, so Pipeline and Executor can never oversubscribe the
  // database-wide worker budget.
  client.config_.max_threads_ = std::clamp<idx_t>(database_->WorkerCapacity(), 1, MAX_THREADS);
  client.mem_.SetBudget(config.max_memory_);
  client.mem_.SetGlobalManager(&database_->GlobalMemoryManager());
}

auto Connection::AcquireQueryWorkers(ClientContext &client, idx_t demand) -> WorkerSlotManager::Token {
  const idx_t capacity = std::clamp<idx_t>(database_->WorkerCapacity(), 1, MAX_THREADS);
  const idx_t base = std::clamp<idx_t>(current_worker_slots_, 1, capacity);
  const idx_t desired = std::clamp<idx_t>(demand, 1, capacity);
  auto additional = database_->TryAcquireWorkerSlots(desired > base ? desired - base : 0);
  client.config_.max_threads_ = base + additional.Slots();
  return additional;
}

auto Connection::EvalScalarSubquery(const AbstractPlanNodeRef &subplan) -> Value {
  const auto result_type = subplan->OutputSchema().GetColumn(0).GetType();

  // LIMIT 2 caps the materialization: 0/1 rows are the legal outcomes and a second row already
  // proves the error, so a huge accidental result is never fully collected just to be rejected.
  auto limited = std::make_shared<LimitPlanNode>(subplan->output_schema_, subplan, 2);
  Optimizer optimizer(*catalog_);
  auto optimized = optimizer.Optimize(limited);

  // Same transaction discipline as ExecuteStatement: join the session's explicit transaction when
  // one is open, else autocommit — with the same detect → force-external → retry loop on overflow.
  const bool autocommit = ActiveTxn() == nullptr;
  std::unordered_set<const AbstractPlanNode *> force_external;
  while (true) {
    ClientContext client(*catalog_, *txn_mgr_, bpm_);
    ApplyConfig(client);
    auto txn = autocommit ? txn_mgr_->BeginShared() : ActiveTxn();
    TransactionActivityGuard activity(txn);
    client.txn_ = txn.get();
    try {
      PhysicalPlanGenerator generator(client);
      generator.SetForceExternal(force_external);
      auto physical = generator.PlanRoot(optimized);

      Executor executor(client);
      executor.Initialize(*physical);
      auto query_workers = AcquireQueryWorkers(client, executor.PeakTaskDemand());
      executor.ExecuteQuery();

      auto *gs = dynamic_cast<ResultCollectorGlobalState *>(executor.GetOrCreateSinkState(*physical));
      Value result = Value::Null(result_type);
      idx_t rows = 0;
      if (gs != nullptr) {
        for (const auto &chunk : gs->chunks_) {
          if (chunk->GetSize() > 0 && rows == 0) {
            result = chunk->GetValue(0, 0);  // Value owns its payload; safe past the executor
          }
          rows += chunk->GetSize();
        }
      }
      if (rows > 1) {
        throw ExecutionException("scalar subquery returned more than one row");
      }
      txn->ThrowIfCancellationRequested();
      if (autocommit) {
        txn_mgr_->Commit(txn.get());
      }
      return result;
    } catch (const MemoryLimitException &e) {
      const auto *culprit = static_cast<const AbstractPlanNode *>(e.Culprit());
      if (autocommit && culprit != nullptr && force_external.insert(culprit).second) {
        txn_mgr_->Abort(txn.get());
        continue;
      }
      if (autocommit) {
        txn_mgr_->Abort(txn.get());
      } else {
        AbortActiveTransaction();
      }
      throw;
    } catch (...) {
      if (autocommit) {
        txn_mgr_->Abort(txn.get());
      } else {
        AbortActiveTransaction();  // a failed statement aborts the explicit transaction
      }
      throw;
    }
  }
}

auto Connection::ExecuteStatement(const BoundStatement &statement) -> QueryResult {
  // Optimize ONCE; only lowering (below) decides in-memory vs external, so the logical tree is a stable
  // set of nodes across retries and each node is identified by its pointer. Scalar subqueries are
  // pre-executed during planning (via the eval hook) for the same reason: substituting them later
  // would rebuild the tree and break the pointer identity the retry loop depends on.
  Planner planner(*catalog_);
  planner.subquery_eval_ = [this](const AbstractPlanNodeRef &subplan) { return EvalScalarSubquery(subplan); };
  planner.PlanQuery(statement);
  Optimizer optimizer(*catalog_);
  auto optimized_plan = optimizer.Optimize(planner.plan_);

  // Autocommit: without an explicit BEGIN, each statement runs in its own transaction. Inside an
  // explicit transaction (the session's ActiveTxn() set) the statement joins it — it is neither committed
  // here nor retried under a fresh transaction; a failure aborts the whole transaction instead.
  const bool autocommit = ActiveTxn() == nullptr;

  // External parquet tables commit by manifest swap, outside MVCC: a ROLLBACK could never undo the
  // file rewrite, so a write to one inside an explicit transaction is refused up front (mirroring
  // how DDL is non-transactional) instead of silently ignoring the transaction.
  if (!autocommit) {
    const BoundBaseTableRef *target = nullptr;
    if (statement.type_ == StatementType::INSERT_STATEMENT) {
      target = dynamic_cast<const InsertStatement &>(statement).table_.get();
    } else if (statement.type_ == StatementType::UPDATE_STATEMENT) {
      target = dynamic_cast<const UpdateStatement &>(statement).table_.get();
    } else if (statement.type_ == StatementType::DELETE_STATEMENT) {
      target = dynamic_cast<const DeleteStatement &>(statement).table_.get();
    }
    if (target != nullptr) {
      auto info = catalog_->GetTable(target->oid_);
      if (info != NULL_TABLE_INFO && info->storage_ != nullptr &&
          info->storage_->GetFormat() == StorageFormat::PARQUET) {
        throw Exception(fmt::format(
            "external table '{}' does not support transactional writes: run the statement outside BEGIN/COMMIT",
            target->table_));
      }
    }
  }

  // Detect → re-plan → retry: an in-memory breaker that overflows the budget throws MemoryLimitException
  // naming its logical node; in autocommit mode we abort, force just that node external, and re-run under
  // a fresh transaction. That retry needs a clean rollback, which we cannot do for a single statement of
  // an explicit transaction — so there we surface the overflow (and abort the transaction) instead.
  std::unordered_set<const AbstractPlanNode *> force_external;
  while (true) {
    // A fresh ClientContext gives each attempt a fresh memory budget.
    ClientContext client(*catalog_, *txn_mgr_, bpm_);
    ApplyConfig(client);
    auto txn = autocommit ? txn_mgr_->BeginShared() : ActiveTxn();
    TransactionActivityGuard activity(txn);
    client.txn_ = txn.get();
    try {
      PhysicalPlanGenerator generator(client);
      generator.SetForceExternal(force_external);
      auto physical = generator.PlanRoot(optimized_plan);

      Executor executor(client);
      executor.Initialize(*physical);
      auto query_workers = AcquireQueryWorkers(client, executor.PeakTaskDemand());
      executor.ExecuteQuery();

      auto *collector = dynamic_cast<ResultCollectorGlobalState *>(executor.GetOrCreateSinkState(*physical));
      data_chunk_vector_t chunks;
      if (collector != nullptr) {
        chunks = collector->TakeChunks();
      }
      auto result = QueryResult::Rows(*physical->output_schema_, std::move(chunks), statement.type_);
      txn->ThrowIfCancellationRequested();
      if (autocommit) {
        txn_mgr_->Commit(txn.get());
      }
      return result;
    } catch (const MemoryLimitException &e) {
      const auto *culprit = static_cast<const AbstractPlanNode *>(e.Culprit());
      // Retry (fresh transaction, culprit forced external) only in autocommit mode, and only while we can
      // still make progress — a null or already-forced culprit means give up.
      if (autocommit && culprit != nullptr && force_external.insert(culprit).second) {
        txn_mgr_->Abort(txn.get());  // fresh transaction on the next attempt
        continue;
      }
      if (autocommit) {
        txn_mgr_->Abort(txn.get());
      } else {
        AbortActiveTransaction();
      }
      throw;
    } catch (...) {
      if (autocommit) {
        txn_mgr_->Abort(txn.get());  // roll back partial writes before surfacing
      } else {
        AbortActiveTransaction();  // a failed statement aborts the explicit transaction
      }
      throw;
    }
  }
}

auto Connection::ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool {
  if (sql.empty() || sql[0] != '\\') {
    for (const auto &result : ExecuteSqlResults(sql)) {
      RenderQueryResult(result, writer);
    }
    return true;
  }

  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);

  if (sql == "\\dt") {
    std::optional<DatabaseInstance::SharedSchemaLease> lease;
    if (!explicit_schema_lease_.has_value()) {
      lease.emplace(database_->AcquireSharedSchemaLease());
    }
    CmdDisplayTables(writer);
    return true;
  }
  if (sql == "\\help") {
    CmdDisplayHelp(writer);
    return true;
  }
  if (sql == "\\clear") {
    if (active_txn_ != nullptr) {
      throw ProgrammingException("DDL and clear are not supported inside an explicit transaction");
    }
    auto lease = database_->AcquireExclusiveSchemaLease();
    CmdClear(writer);
    return true;
  }
  // `\d <table>` describes one table's schema. Checked after `\dt` (an exact match handled above), and
  // the required space keeps the two from colliding.
  static const std::string kDescribe = "\\d ";
  if (sql.rfind(kDescribe, 0) == 0) {
    std::optional<DatabaseInstance::SharedSchemaLease> lease;
    if (!explicit_schema_lease_.has_value()) {
      lease.emplace(database_->AcquireSharedSchemaLease());
    }
    auto table = sql.substr(kDescribe.size());
    StringUtil::LTrim(&table);
    StringUtil::RTrim(&table);
    CmdDescribeTable(table, writer);
    return true;
  }
  // `\pipelines <sql>` is a thin alias for EXPLAIN (pipelines) <sql>. This call stays inside the
  // already-admitted operation/connection guard and enters only the internal statement dispatcher.
  static const std::string kPipelines = "\\pipelines ";
  if (sql.rfind(kPipelines, 0) == 0) {
    for (const auto &result : ExecuteSqlResultsInternal("EXPLAIN (pipelines) " + sql.substr(kPipelines.size()))) {
      RenderQueryResult(result, writer);
    }
    return true;
  }
  // `\gc` drives TransactionManager::GarbageCollection() — the transaction-timeout enforcer — on
  // demand. A shared schema lease keeps timeout rollback from racing table reclamation.
  if (sql == "\\gc") {
    std::optional<DatabaseInstance::SharedSchemaLease> lease;
    if (!explicit_schema_lease_.has_value()) {
      lease.emplace(database_->AcquireSharedSchemaLease());
    }
    CmdGarbageCollect(writer);
    return true;
  }
  // `\vacuum <table>` sweeps an external table's folder: files not referenced by the newest
  // manifest (crash leftovers) and superseded manifest versions.
  static const std::string kVacuum = "\\vacuum ";
  if (sql.rfind(kVacuum, 0) == 0) {
    if (active_txn_ != nullptr) {
      throw ProgrammingException("vacuum is not supported inside an explicit transaction");
    }
    auto lease = database_->AcquireExclusiveSchemaLease();
    auto name = sql.substr(kVacuum.size());
    StringUtil::LTrim(&name);
    StringUtil::RTrim(&name);
    CmdVacuumExternal(name, writer);
    return true;
  }
  throw ProgrammingException(fmt::format("unsupported meta-command: {}", sql));
}

void Connection::BeginTransaction(IsolationLevel isolation_level) {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  BeginTransactionInternal(isolation_level);
}

void Connection::CommitTransaction() {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  CommitTransactionInternal();
}

void Connection::RollbackTransaction() {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  RollbackTransactionInternal();
}

auto Connection::DescribeTableInternal(const std::shared_ptr<TableInfo> &table) const -> TableMetadata {
  TableMetadata metadata;
  metadata.name_ = table->name_;
  metadata.generated_id_ = table->auto_id_;
  if (table->storage_ != nullptr) {
    metadata.storage_format_ = table->storage_->GetFormat();
    metadata.estimated_rows_ = table->storage_->EstimatedRowCount();
    if (const auto *parquet = dynamic_cast<const ParquetTable *>(table->storage_.get()); parquet != nullptr) {
      metadata.location_ = parquet->GetPath();
    }
  }

  std::unordered_set<uint32_t> primary_key(table->pk_attrs_.begin(), table->pk_attrs_.end());
  metadata.columns_.reserve(table->schema_.GetColumnCount());
  metadata.primary_key_.reserve(table->pk_attrs_.size());
  for (uint32_t column = 0; column < table->schema_.GetColumnCount(); column++) {
    const auto &catalog_column = table->schema_.GetColumn(column);
    const bool is_primary_key = primary_key.contains(column);
    metadata.columns_.push_back({catalog_column.GetName(), catalog_column.GetType(), is_primary_key});
    if (is_primary_key) {
      metadata.primary_key_.push_back(catalog_column.GetName());
    }
  }
  return metadata;
}

auto Connection::ListTables() -> std::vector<TableMetadata> {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  std::optional<DatabaseInstance::SharedSchemaLease> lease;
  if (!explicit_schema_lease_.has_value()) {
    lease.emplace(database_->AcquireSharedSchemaLease());
  }

  auto tables = catalog_->GetTables();
  std::vector<TableMetadata> result;
  result.reserve(tables.size());
  for (const auto &table : tables) {
    result.push_back(DescribeTableInternal(table));
  }
  std::sort(result.begin(), result.end(),
            [](const TableMetadata &left, const TableMetadata &right) { return left.name_ < right.name_; });
  return result;
}

auto Connection::DescribeTable(const std::string &table_name) -> TableMetadata {
  ValidateTableHelperName(table_name);
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  std::optional<DatabaseInstance::SharedSchemaLease> lease;
  if (!explicit_schema_lease_.has_value()) {
    lease.emplace(database_->AcquireSharedSchemaLease());
  }
  auto table = catalog_->GetTable(table_name);
  if (table == NULL_TABLE_INFO) {
    throw BinderException(fmt::format("table '{}' does not exist", table_name));
  }
  return DescribeTableInternal(table);
}

auto Connection::VacuumTable(const std::string &table_name) -> size_t {
  ValidateTableHelperName(table_name);
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  if (active_txn_ != nullptr) {
    throw ProgrammingException("vacuum is not supported inside an explicit transaction");
  }
  auto lease = database_->AcquireExclusiveSchemaLease();
  return VacuumTableInternal(table_name);
}

auto Connection::GarbageCollect() -> TransactionManager::GcStats {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  std::optional<DatabaseInstance::SharedSchemaLease> lease;
  if (!explicit_schema_lease_.has_value()) {
    lease.emplace(database_->AcquireSharedSchemaLease());
  }
  return GarbageCollectInternal();
}

auto Connection::ExecuteSqlResults(const std::string &sql) -> std::vector<QueryResult> {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  try {
    return ExecuteSqlResultsInternal(sql);
  } catch (...) {
    if (active_txn_ != nullptr && active_txn_->IsCancellationRequested()) {
      AbortActiveTransaction();
    }
    throw;
  }
}

auto Connection::ExecuteSqlScript(const std::string &sql) -> std::vector<QueryResult> {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  if (sql.find('\0') != std::string::npos) {
    throw ParserException("SQL text must not contain NUL bytes");
  }
  const bool transaction_was_active = active_txn_ != nullptr;
  try {
    auto results = ExecuteSqlResultsInternal(sql, true);
    if (!transaction_was_active && active_txn_ != nullptr) {
      AbortActiveTransaction();
      throw ProgrammingException("script statement end: transaction left open; it was rolled back");
    }
    return results;
  } catch (...) {
    if (!transaction_was_active && active_txn_ != nullptr) {
      AbortActiveTransaction();
    } else if (active_txn_ != nullptr && active_txn_->IsCancellationRequested()) {
      AbortActiveTransaction();
    }
    throw;
  }
}

auto Connection::ExecuteSqlStatement(const std::string &sql, bool allow_transaction_control) -> QueryResult {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);

  // libpg_query accepts a C string and would otherwise silently stop at an embedded NUL. Reject it
  // before parsing so Python/other length-aware frontends cannot accidentally execute a prefix.
  if (sql.find('\0') != std::string::npos) {
    throw ParserException("SQL text must not contain NUL bytes");
  }

  // Validate cardinality and Database.sql's autocommit-only policy before dispatching anything. The
  // internal path parses again; this small frontend cost keeps the existing script dispatcher simple
  // while guaranteeing that a rejected multi-statement string has no partial side effects.
  {
    // libpg_query's parse tree owns parser-allocator state, so destroy this validation Binder before
    // the dispatcher constructs the Binder that performs the real bind/execute pass.
    Binder validator(*catalog_);
    validator.ParseAndSave(sql);
    if (validator.statement_nodes_.size() != 1) {
      throw ProgrammingException("sql() requires exactly one SQL statement; use execute_script() for scripts");
    }
    if (!allow_transaction_control &&
        RawStatementType(validator.statement_nodes_.front()) == StatementType::TRANSACTION_STATEMENT) {
      throw ProgrammingException("transaction-control statements require Database.connect()");
    }
  }

  try {
    auto results = ExecuteSqlResultsInternal(sql);
    return std::move(results.front());
  } catch (...) {
    if (active_txn_ != nullptr && active_txn_->IsCancellationRequested()) {
      AbortActiveTransaction();
    }
    throw;
  }
}

auto Connection::LoadDataChunks(std::string table_name, std::vector<std::string> column_names,
                                std::vector<LogicalType> column_types, std::vector<std::string> primary_key,
                                bool append, data_chunk_vector_t chunks) -> idx_t {
  auto operation = DatabaseInstance::AcquireOperation(database_);
  StatementGuard statement_guard(*this);
  auto worker_slot = database_->AcquireWorkerSlot();
  if (active_txn_ != nullptr) {
    throw ProgrammingException("load_df is not supported inside an explicit transaction");
  }
  if (table_name.empty() || table_name.find('\0') != std::string::npos) {
    throw DataException("table name must be a non-empty string without NUL bytes");
  }
  if (column_names.empty() || column_names.size() != column_types.size()) {
    throw DataException("load data requires at least one named column and matching logical types");
  }

  std::unordered_set<std::string> names;
  for (const auto &name : column_names) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      throw DataException("column names must be non-empty strings without NUL bytes");
    }
    if (StringUtil::Lower(name) == AUTO_ID_COLUMN) {
      throw DataException(fmt::format("column name '{}' is reserved for the generated key", AUTO_ID_COLUMN));
    }
    if (!names.insert(name).second) {
      throw DataException(fmt::format("duplicate DataFrame column name '{}'", name));
    }
  }

  std::vector<uint32_t> requested_pk;
  std::unordered_set<std::string> key_names;
  for (const auto &key : primary_key) {
    if (!key_names.insert(key).second) {
      throw DataException(fmt::format("duplicate primary-key column '{}'", key));
    }
    auto found = std::find(column_names.begin(), column_names.end(), key);
    if (found == column_names.end()) {
      throw DataException(fmt::format("primary-key column '{}' is not in the DataFrame", key));
    }
    const auto attr = static_cast<uint32_t>(std::distance(column_names.begin(), found));
    if (!column_types[attr].IsConstantSize()) {
      throw DataException(fmt::format("primary-key column '{}' has unsupported variable-length type {}", key,
                                      column_types[attr].ToString()));
    }
    requested_pk.push_back(attr);
  }

  idx_t total_rows = 0;
  for (const auto &chunk : chunks) {
    if (chunk == nullptr || chunk->ColumnCount() != column_types.size() || chunk->GetTypes() != column_types) {
      throw DataException("owned input chunk does not match the inferred DataFrame schema");
    }
    for (const auto key_column : requested_pk) {
      for (idx_t row = 0; row < chunk->GetSize(); row++) {
        if (chunk->GetValue(key_column, row).IsNull()) {
          throw DataException(fmt::format("primary-key column '{}' contains NULL", column_names[key_column]));
        }
      }
    }
    total_rows += chunk->GetSize();
  }

  auto validate_existing = [&](const TableInfo &table) {
    if (table.storage_ == nullptr || table.storage_->GetFormat() != StorageFormat::ROW) {
      throw DataException(fmt::format("cannot append a DataFrame to non-row table '{}'", table_name));
    }
    const idx_t offset = table.auto_id_ ? 1 : 0;
    if (table.schema_.GetColumnCount() != column_names.size() + offset) {
      throw DataException(fmt::format("DataFrame schema does not match existing table '{}'", table_name));
    }
    for (idx_t column = 0; column < column_names.size(); column++) {
      const auto &target = table.schema_.GetColumn(column + offset);
      if (target.GetName() != column_names[column] || target.GetType() != column_types[column]) {
        throw DataException(fmt::format("DataFrame schema does not match existing table '{}'", table_name));
      }
    }
    if (!primary_key.empty()) {
      std::vector<uint32_t> expected = requested_pk;
      for (auto &attr : expected) {
        attr += static_cast<uint32_t>(offset);
      }
      if (expected != table.pk_attrs_) {
        throw DataException(fmt::format("primary_key does not match existing table '{}'", table_name));
      }
    }
  };

  auto insert_all = [&](TableInfo &table) {
    auto transaction = txn_mgr_->BeginShared();
    try {
      TransactionActivityGuard activity(transaction);
      for (auto &chunk : chunks) {
        transaction->ThrowIfCancellationRequested();
        InsertTableChunk(*catalog_, txn_mgr_, transaction.get(), table, *chunk);
      }
      transaction->ThrowIfCancellationRequested();
      if (!txn_mgr_->Commit(transaction.get())) {
        throw ConflictException("bulk load failed serializable validation");
      }
    } catch (...) {
      const auto state = transaction->GetTransactionState();
      if (state == TransactionState::RUNNING || state == TransactionState::TAINTED) {
        txn_mgr_->Abort(transaction.get());
      }
      throw;
    }
  };

  // Existing-table append only needs the ordinary shared DML lease and can overlap unrelated work.
  {
    auto shared_schema = database_->AcquireSharedSchemaLease();
    auto existing = catalog_->GetTable(table_name);
    if (existing != nullptr) {
      if (!append) {
        throw DataException(fmt::format("table '{}' already exists", table_name));
      }
      validate_existing(*existing);
      insert_all(*existing);
      return total_rows;
    }
  }

  // Creating a table is schema-exclusive. Recheck after upgrading the lease so two racing creators
  // have one deterministic winner; an append caller may use the winner after exact schema validation.
  auto exclusive_schema = database_->AcquireExclusiveSchemaLease();
  if (auto existing = catalog_->GetTable(table_name); existing != nullptr) {
    if (!append) {
      throw DataException(fmt::format("table '{}' already exists", table_name));
    }
    validate_existing(*existing);
    insert_all(*existing);
    return total_rows;
  }

  std::vector<Column> columns;
  std::vector<uint32_t> table_pk;
  const bool auto_id = primary_key.empty();
  if (auto_id) {
    columns.push_back(Column::Make(AUTO_ID_COLUMN, LogicalType(LogicalTypeId::BIGINT)));
    table_pk.push_back(0);
  }
  columns.reserve(column_names.size() + (auto_id ? 1 : 0));
  for (idx_t column = 0; column < column_names.size(); column++) {
    columns.push_back(Column::Make(column_names[column], column_types[column]));
  }
  if (!auto_id) {
    table_pk = requested_pk;
  }

  bool created = false;
  try {
    auto table = catalog_->CreateTable(table_name, Schema(columns), StorageFormat::ROW, table_pk, auto_id);
    if (table == nullptr) {
      throw DataException(fmt::format("table '{}' already exists", table_name));
    }
    created = true;
    if (!table_pk.empty() && table->storage_ != nullptr) {
      if (catalog_->CreateIndexForKey("_pk_" + table_name, table_name, table_pk) == nullptr) {
        throw ExecutionException(fmt::format("failed to create primary-key index for '{}'", table_name));
      }
    }
    insert_all(*table);
  } catch (...) {
    if (created) {
      catalog_->DropTable(table_name);
    }
    throw;
  }
  return total_rows;
}

auto Connection::ExecuteSqlResultsInternal(const std::string &sql, bool annotate_statement_errors)
    -> std::vector<QueryResult> {
  if (!sql.empty() && sql[0] == '\\') {
    throw ProgrammingException("shell meta-commands do not produce native QueryResult objects");
  }
  Binder binder(*catalog_);
  try {
    binder.ParseAndSave(sql);
  } catch (const Exception &error) {
    if (!annotate_statement_errors) {
      throw;
    }
    throw Exception(
        error.GetType(),
        fmt::format("script statement {}: {}", ScriptStatementIndex(sql, binder.ParserErrorLocation()), error.what()));
  }

  std::vector<QueryResult> results;
  results.reserve(binder.statement_nodes_.size());
  for (size_t statement_index = 0; statement_index < binder.statement_nodes_.size(); statement_index++) {
    auto *raw_statement = binder.statement_nodes_[statement_index];
    try {
      const auto raw_type = RawStatementType(raw_statement);
      const bool ddl = raw_type == StatementType::CREATE_STATEMENT || raw_type == StatementType::DROP_STATEMENT;
      if (ddl && active_txn_ != nullptr) {
        throw ProgrammingException("DDL is not supported inside an explicit transaction");
      }

      std::optional<WorkerSlotManager::Token> worker_slot;
      if (raw_type != StatementType::TRANSACTION_STATEMENT && !ddl) {
        worker_slot.emplace(database_->AcquireWorkerSlot());
        current_worker_slots_ = worker_slot->Slots();
      }

      std::optional<DatabaseInstance::SharedSchemaLease> shared_lease;
      std::optional<DatabaseInstance::ExclusiveSchemaLease> exclusive_lease;
      if (ddl) {
        exclusive_lease.emplace(database_->AcquireExclusiveSchemaLease());
      } else if (raw_type != StatementType::TRANSACTION_STATEMENT && !explicit_schema_lease_.has_value()) {
        shared_lease.emplace(database_->AcquireSharedSchemaLease());
      }

      // An explicit transaction is considered active during binding as well as execution. Timeout GC
      // can mark it cancelled, but never rolls its write set back until this statement reaches an exit.
      std::optional<TransactionActivityGuard> transaction_activity;
      if (active_txn_ != nullptr && raw_type != StatementType::TRANSACTION_STATEMENT) {
        transaction_activity.emplace(active_txn_);
      }

      auto statement = binder.BindStatement(raw_statement);
      if (active_txn_ != nullptr && raw_type != StatementType::TRANSACTION_STATEMENT) {
        active_txn_->ThrowIfCancellationRequested();
      }

      switch (statement->type_) {
        case StatementType::CREATE_STATEMENT: {
          results.push_back(HandleCreateStatement(dynamic_cast<const CreateStatement &>(*statement)));
          continue;
        }
        case StatementType::DROP_STATEMENT: {
          results.push_back(HandleDropStatement(dynamic_cast<const DropStatement &>(*statement)));
          continue;
        }
        case StatementType::TRANSACTION_STATEMENT: {
          results.push_back(HandleTransactionStatement(dynamic_cast<const TransactionStatement &>(*statement)));
          continue;
        }
        case StatementType::EXPLAIN_STATEMENT: {
          results.push_back(HandleExplainStatement(dynamic_cast<const ExplainStatement &>(*statement)));
          continue;
        }
        default:
          break;
      }

      results.push_back(ExecuteStatement(*statement));
    } catch (const Exception &error) {
      if (!annotate_statement_errors) {
        throw;
      }
      throw Exception(error.GetType(), fmt::format("script statement {}: {}", statement_index + 1, error.what()));
    }
  }

  return results;
}

}  // namespace bumblebee
