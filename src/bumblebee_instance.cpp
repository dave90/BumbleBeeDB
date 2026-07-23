//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumblebee_instance.cpp
//
// Identification: src/bumblebee_instance.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "bumblebee_instance.h"

#include <algorithm>
#include <memory>
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
#include "execution/physical_plan_generator.h"
#include "execution/plans/abstract_plan.h"
#include "fmt/format.h"
#include "main/client_context.h"
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

/** Backing store size for an in-memory instance: 4096 pages (32 MiB) is plenty for the test suites. */
constexpr size_t kInMemoryDiskPages = 4096;
constexpr size_t kInMemoryFrames = 1024;

/** @brief Stream a result collector's gathered chunks to `writer` as a table. */
void WriteResultTable(const PhysicalOperator &root, GlobalSinkState *sink_state, ResultWriter &writer) {
  auto *gs = dynamic_cast<ResultCollectorGlobalState *>(sink_state);
  writer.BeginTable(true);
  writer.BeginHeader();
  for (const auto &col : root.output_schema_->GetColumns()) {
    writer.WriteHeaderCell(col.GetName());
  }
  writer.EndHeader();
  if (gs != nullptr) {
    // A writer may cap how many rows it wants (the interactive shell does, to avoid flooding the
    // terminal on a huge SELECT); 0 means unlimited. We emit up to the cap, then report the total.
    const idx_t limit = writer.MaxDisplayRows();
    idx_t total = 0;
    for (const auto &chunk : gs->chunks_) {
      total += chunk->GetSize();
    }
    idx_t shown = 0;
    for (const auto &chunk : gs->chunks_) {
      if (limit != 0 && shown >= limit) {
        break;
      }
      for (idx_t r = 0; r < chunk->GetSize(); r++) {
        if (limit != 0 && shown >= limit) {
          break;
        }
        writer.BeginRow();
        for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
          writer.WriteCell(chunk->GetValue(c, r).ToString());
        }
        writer.EndRow();
        shown++;
      }
    }
    writer.EndTable();
    if (limit != 0 && total > limit) {
      writer.WriteTruncationNotice(shown, total);
    }
    return;
  }
  writer.EndTable();
}

}  // namespace

BumbleBeeInstance::BumbleBeeInstance(duration_t txn_timeout)
    : mem_disk_(std::make_unique<MemoryDiskManager>(kInMemoryDiskPages)),
      owned_bpm_(std::make_unique<BufferPoolManager>(kInMemoryFrames, mem_disk_.get())),
      owned_catalog_(std::make_unique<Catalog>(owned_bpm_.get())),
      owned_txn_mgr_(std::make_unique<TransactionManager>(owned_catalog_.get(), txn_timeout)) {
  catalog_ = owned_catalog_.get();
  bpm_ = owned_bpm_.get();
  txn_mgr_ = owned_txn_mgr_.get();
}

BumbleBeeInstance::BumbleBeeInstance(const std::filesystem::path &db_file, size_t num_frames,
                                     duration_t txn_timeout)
    : db_(std::make_unique<Database>(db_file, num_frames, txn_timeout)) {
  // The catalog, buffer pool and transaction manager all live inside the Database (which owns the disk);
  // ~Database (via the db_ member) flushes and persists on shutdown.
  catalog_ = &db_->GetCatalog();
  bpm_ = &db_->GetBufferPool();
  txn_mgr_ = &db_->GetTransactionManager();
}

BumbleBeeInstance::~BumbleBeeInstance() {
  // Roll back every session's explicit transaction left open at shutdown so no uncommitted write persists.
  for (auto &[name, txn] : session_txns_) {
    if (txn != nullptr) {
      txn_mgr_->Abort(txn);
      txn = nullptr;
    }
  }
}

void BumbleBeeInstance::WriteOneCell(const std::string &cell, ResultWriter &writer) { writer.OneCell(cell); }

void BumbleBeeInstance::GenerateMockTable() {
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

void BumbleBeeInstance::CmdDisplayTables(ResultWriter &writer) {
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

void BumbleBeeInstance::CmdDescribeTable(const std::string &table_name, ResultWriter &writer) {
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

void BumbleBeeInstance::CmdClear(ResultWriter &writer) {
  const auto dropped = catalog_->DropAllTables();
  WriteOneCell(fmt::format("Cleared the database: dropped {} table(s).", dropped), writer);
}

void BumbleBeeInstance::CmdGarbageCollect(ResultWriter &writer) {
  // Snapshot the id of every session's open transaction BEFORE running GC: the sweep *destroys*
  // reclaimed transactions, so afterwards a stored pointer whose txn timed out must not be
  // dereferenced — the sessions are reconciled through the manager by id instead.
  std::vector<std::pair<std::string, txn_id_t>> open;
  for (const auto &[name, txn] : session_txns_) {
    if (txn != nullptr) {
      open.emplace_back(name, txn->GetTransactionId());
    }
  }

  const auto stats = txn_mgr_->GarbageCollection();

  // A session whose transaction is gone from the manager (or left ABORTED) was timed out by this
  // pass; drop the session back to autocommit so its next statement does not touch a dead txn.
  for (const auto &[name, id] : open) {
    const auto state = txn_mgr_->GetTransactionState(id);
    if (!state.has_value() || *state == TransactionState::ABORTED) {
      session_txns_[name] = nullptr;
    }
  }

  // Only the timeout-abort count is reported: it is what the caller (and the timeout tests) can
  // predict, while the reclaimed count depends on the whole history of finished transactions.
  WriteOneCell(fmt::format("GC: aborted {} timed-out transaction(s)", stats.timed_out_), writer);
}

void BumbleBeeInstance::CmdDisplayHelp(ResultWriter &writer) {
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

void BumbleBeeInstance::HandleCreateStatement(const CreateStatement &stmt, ResultWriter &writer) {
  if (stmt.format_ == StorageFormat::PARQUET) {
    HandleCreateExternalTable(stmt, writer);
    return;
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
  WriteOneCell(fmt::format("Table created with id = {}", info->oid_), writer);
}

void BumbleBeeInstance::HandleCreateExternalTable(const CreateStatement &stmt, ResultWriter &writer) {
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
      throw Exception(fmt::format(
          "external table '{}': parquet file '{}' does not match the {} schema", stmt.table_, entry.file_name_,
          stmt.columns_.empty() ? "inferred" : "declared"));
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
  WriteOneCell(fmt::format("External table created with id = {} at '{}'", info->oid_, location), writer);
}

void BumbleBeeInstance::CmdVacuumExternal(const std::string &table_name, ResultWriter &writer) {
  namespace fs = std::filesystem;
  auto info = catalog_->GetTable(table_name);
  if (info == NULL_TABLE_INFO) {
    throw Exception(fmt::format("no such table: {}", table_name));
  }
  auto *parquet = dynamic_cast<ParquetTable *>(info->storage_.get());
  if (parquet == nullptr) {
    throw Exception(fmt::format("\\vacuum only applies to external parquet tables ('{}' is row-format)",
                                table_name));
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
    const bool old_manifest =
        name.starts_with(ParquetManifestIO::MANIFEST_PREFIX) && name != newest_manifest;
    if (orphan_part || old_manifest) {
      std::error_code ec;
      if (fs::remove(entry.path(), ec)) {
        removed++;
      }
    }
  }
  WriteOneCell(fmt::format("Vacuumed {} file(s) from '{}'", removed, dir), writer);
}

void BumbleBeeInstance::HandleDropStatement(const DropStatement &stmt, ResultWriter &writer) {
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
  WriteOneCell(fmt::format("Dropped {} table(s)", dropped), writer);
}

void BumbleBeeInstance::HandleTransactionStatement(const TransactionStatement &stmt, ResultWriter &writer) {
  auto &active_txn = ActiveTxn();
  switch (stmt.txn_type_) {
    case TransactionType::BEGIN:
      // Nested BEGIN is an error — there are no savepoints, so a second BEGIN would silently leak the
      // first transaction.
      if (active_txn != nullptr) {
        throw Exception("cannot BEGIN: a transaction is already in progress");
      }
      active_txn = txn_mgr_->Begin();
      break;
    case TransactionType::COMMIT:
      if (active_txn == nullptr) {
        throw Exception("cannot COMMIT: no transaction is in progress");
      }
      if (!txn_mgr_->Commit(active_txn)) {
        active_txn = nullptr;
        throw ExecutionException("cannot COMMIT: serializable validation failed, transaction aborted");
      }
      active_txn = nullptr;
      break;
    case TransactionType::ROLLBACK:
      if (active_txn == nullptr) {
        throw Exception("cannot ROLLBACK: no transaction is in progress");
      }
      txn_mgr_->Abort(active_txn);
      active_txn = nullptr;
      break;
  }
  WriteOneCell(TransactionTypeToString(stmt.txn_type_), writer);
}

void BumbleBeeInstance::HandleExplainStatement(const ExplainStatement &stmt, ResultWriter &writer) {
  std::string output;

  if ((stmt.options_ & ExplainOptions::BINDER) != 0) {
    output += "=== BINDER ===\n";
    output += stmt.statement_->ToString();
    output += "\n";
  }

  Planner planner(*catalog_);
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
    auto *txn = autocommit ? txn_mgr_->Begin() : ActiveTxn();
    client.txn_ = txn;
    try {
      PhysicalPlanGenerator generator(client);
      auto physical = generator.PlanRoot(optimized_plan);

      if (want_physical) {
        output += "=== PHYSICAL ===\n" + physical->ToString() + "\n";
      }
      if (want_pipelines || want_analyze) {
        Executor executor(client);
        executor.Initialize(*physical);
        if (want_analyze) {
          executor.ExecuteQuery();  // EXPLAIN ANALYZE actually runs the query
          output += "=== ANALYZE ===\n" + executor.AnalyzeToString(*physical) + "\n";
        }
        if (want_pipelines) {
          output += "=== PIPELINES ===\n" + executor.PipelinesToString() + "\n";
        }
      }
      if (autocommit) {
        txn_mgr_->Commit(txn);
      }
    } catch (...) {
      txn_mgr_->Abort(txn);
      if (!autocommit) {
        ActiveTxn() = nullptr;  // a failed statement aborts the explicit transaction
      }
      throw;
    }
  }

  WriteOneCell(output, writer);
}

void BumbleBeeInstance::ApplyConfig(ClientContext &client) const {
  client.config_.prefer_external_ = prefer_external_;
  client.config_.max_memory_ = max_memory_;
  client.config_.morsel_pages_ = std::max<idx_t>(1, morsel_pages_);
  client.config_.morsel_size_ = morsel_size_;
  // 0 means "leave the ClientContext's hardware-detected default"; otherwise clamp to the hard ceiling.
  if (max_threads_ > 0) {
    client.config_.max_threads_ = std::clamp<idx_t>(max_threads_, 1, MAX_THREADS);
  }
  client.mem_.SetBudget(max_memory_);
}

void BumbleBeeInstance::ExecuteStatement(const BoundStatement &statement, ResultWriter &writer) {
  // Optimize ONCE; only lowering (below) decides in-memory vs external, so the logical tree is a stable
  // set of nodes across retries and each node is identified by its pointer.
  Planner planner(*catalog_);
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
    auto *txn = autocommit ? txn_mgr_->Begin() : ActiveTxn();
    client.txn_ = txn;
    try {
      PhysicalPlanGenerator generator(client);
      generator.SetForceExternal(force_external);
      auto physical = generator.PlanRoot(optimized_plan);

      Executor executor(client);
      executor.Initialize(*physical);
      executor.ExecuteQuery();

      WriteResultTable(*physical, executor.GetOrCreateSinkState(*physical), writer);
      if (autocommit) {
        txn_mgr_->Commit(txn);
      }
      return;
    } catch (const MemoryLimitException &e) {
      const auto *culprit = static_cast<const AbstractPlanNode *>(e.Culprit());
      // Retry (fresh transaction, culprit forced external) only in autocommit mode, and only while we can
      // still make progress — a null or already-forced culprit means give up.
      if (autocommit && culprit != nullptr && force_external.insert(culprit).second) {
        txn_mgr_->Abort(txn);  // fresh transaction on the next attempt
        continue;
      }
      txn_mgr_->Abort(txn);
      if (!autocommit) {
        ActiveTxn() = nullptr;
      }
      throw;
    } catch (...) {
      txn_mgr_->Abort(txn);  // roll back partial writes (a conflict or other error) before surfacing
      if (!autocommit) {
        ActiveTxn() = nullptr;  // a failed statement aborts the explicit transaction
      }
      throw;
    }
  }
}

auto BumbleBeeInstance::ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool {
  if (!sql.empty() && sql[0] == '\\') {
    if (sql == "\\dt") {
      CmdDisplayTables(writer);
      return true;
    }
    if (sql == "\\help") {
      CmdDisplayHelp(writer);
      return true;
    }
    if (sql == "\\clear") {
      CmdClear(writer);
      return true;
    }
    // `\d <table>` describes one table's schema. Checked after `\dt` (an exact match handled above), and
    // the required space keeps the two from colliding.
    static const std::string kDescribe = "\\d ";
    if (sql.rfind(kDescribe, 0) == 0) {
      auto table = sql.substr(kDescribe.size());
      StringUtil::LTrim(&table);
      StringUtil::RTrim(&table);
      CmdDescribeTable(table, writer);
      return true;
    }
    // `\pipelines <sql>` is a thin alias for EXPLAIN (pipelines) <sql> — it needs the query bound,
    // planned, optimized and lowered, which the EXPLAIN path already does, and stops short of running.
    static const std::string kPipelines = "\\pipelines ";
    if (sql.rfind(kPipelines, 0) == 0) {
      return ExecuteSql("EXPLAIN (pipelines) " + sql.substr(kPipelines.size()), writer);
    }
    // `\session <name>` switches which named session subsequent statements run in (created on first
    // use). Deliberately SILENT — no output — so the e2e harness can prefix any record with a session
    // switch without polluting the record's expected rows.
    static const std::string kSession = "\\session ";
    if (sql.rfind(kSession, 0) == 0) {
      auto name = sql.substr(kSession.size());
      StringUtil::LTrim(&name);
      StringUtil::RTrim(&name);
      if (name.empty()) {
        throw Exception("\\session requires a session name");
      }
      current_session_ = name;
      return true;
    }
    // `\gc` drives TransactionManager::GarbageCollection() — the transaction-timeout enforcer — on
    // demand. Nothing runs GC automatically yet, so this is how a timed-out transaction gets aborted.
    if (sql == "\\gc") {
      CmdGarbageCollect(writer);
      return true;
    }
    // `\vacuum <table>` sweeps an external table's folder: files not referenced by the newest
    // manifest (crash leftovers) and superseded manifest versions.
    static const std::string kVacuum = "\\vacuum ";
    if (sql.rfind(kVacuum, 0) == 0) {
      auto name = sql.substr(kVacuum.size());
      StringUtil::LTrim(&name);
      StringUtil::RTrim(&name);
      CmdVacuumExternal(name, writer);
      return true;
    }
    throw Exception(fmt::format("unsupported meta-command: {}", sql));
  }

  Binder binder(*catalog_);
  binder.ParseAndSave(sql);

  for (auto *stmt : binder.statement_nodes_) {
    auto statement = binder.BindStatement(stmt);

    switch (statement->type_) {
      case StatementType::CREATE_STATEMENT: {
        HandleCreateStatement(dynamic_cast<const CreateStatement &>(*statement), writer);
        continue;
      }
      case StatementType::DROP_STATEMENT: {
        HandleDropStatement(dynamic_cast<const DropStatement &>(*statement), writer);
        continue;
      }
      case StatementType::TRANSACTION_STATEMENT: {
        HandleTransactionStatement(dynamic_cast<const TransactionStatement &>(*statement), writer);
        continue;
      }
      case StatementType::EXPLAIN_STATEMENT: {
        HandleExplainStatement(dynamic_cast<const ExplainStatement &>(*statement), writer);
        continue;
      }
      default:
        break;
    }

    ExecuteStatement(*statement, writer);
  }

  return true;
}

}  // namespace bumblebee
