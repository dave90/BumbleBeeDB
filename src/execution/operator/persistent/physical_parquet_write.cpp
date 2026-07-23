//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_parquet_write.cpp
//
// Identification: src/execution/operator/persistent/physical_parquet_write.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// The three write operators of external parquet tables. They share one shape:
//   - Sink tasks buffer their contribution lock-free (chunks / RIDs / new rows);
//   - Combine merges into the global sink state under its mutex;
//   - Finalize is the commit point: it takes the fail-fast writer lock (a concurrent writer
//     throws, it never waits), bases itself on the manifest read AFTER the lock, writes fresh
//     part files, and atomically swaps in manifest N+1. Old files are unlinked afterwards —
//     in-flight scans still holding them open keep reading their snapshot (POSIX).
// No MVCC anywhere: the manifest swap IS the commit, regardless of the bookkeeping transaction.
//
//===----------------------------------------------------------------------===//

#include "execution/operator/persistent/physical_parquet_write.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "execution/expression_executor.h"
#include "fmt/format.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/parquet/parquet_reader.h"
#include "storage/parquet/parquet_table_ops.h"
#include "storage/table/parquet_table.h"
#include "type/value.h"

namespace bumblebee {

namespace {

namespace fs = std::filesystem;

auto ResolveParquet(ClientContext &context, table_oid_t oid) -> ParquetTable * {
  auto info = context.catalog_.GetTable(oid);
  if (info == NULL_TABLE_INFO || info->storage_ == nullptr) {
    throw ExecutionException("parquet write: table has no storage backend");
  }
  auto *parquet = dynamic_cast<ParquetTable *>(info->storage_.get());
  if (parquet == nullptr) {
    throw ExecutionException("parquet write: table is not an external parquet table");
  }
  return parquet;
}

auto TableName(ClientContext &context, table_oid_t oid) -> std::string {
  auto info = context.catalog_.GetTable(oid);
  return info != NULL_TABLE_INFO ? info->name_ : fmt::format("oid={}", oid);
}

/** @brief Read the manifest a locked rewrite is based on; a manifest must exist post-CREATE. */
auto ReadManifestOrThrow(const std::string &dir, const std::string &table_name) -> ParquetManifest {
  auto manifest = ParquetManifestIO::ReadLatest(dir);
  if (!manifest.has_value()) {
    throw ExecutionException(
        fmt::format("external table '{}': no manifest found at '{}' (was the folder cleared?)", table_name, dir));
  }
  return std::move(*manifest);
}

/** The RID split: file index in the high 32 bits, row index within the file in the low 32. */
auto RidFileIdx(int64_t rid) -> idx_t { return static_cast<uint64_t>(rid) >> 32U; }
auto RidRowIdx(int64_t rid) -> uint32_t { return static_cast<uint32_t>(static_cast<uint64_t>(rid) & 0xFFFFFFFFU); }

/** @brief Collect the RID column values of a chunk. */
void CollectRids(DataChunk &input, idx_t rid_column, std::vector<int64_t> &out) {
  const idx_t count = input.GetSize();
  input.data_[rid_column].Normalify(count);
  const auto *rd = FlatVector::GetData<int64_t>(input.data_[rid_column]);
  out.insert(out.end(), rd, rd + count);
}

/** @brief The count-emitting source half shared by all three operators. */
struct ParquetWriteSourceState : GlobalSourceState {
  std::atomic<bool> emitted_{false};
  GlobalSinkState *sink_{nullptr};
};

}  // namespace

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

struct ParquetInsertGlobalSinkState : GlobalSinkState {
  std::mutex lock_;
  ChunkCollection rows_;
  idx_t count_{0};
};

struct ParquetInsertLocalSinkState : LocalSinkState {
  ChunkCollection rows_;
};

auto PhysicalParquetInsert::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<ParquetInsertGlobalSinkState>();
}

auto PhysicalParquetInsert::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<ParquetInsertLocalSinkState>();
}

auto PhysicalParquetInsert::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                 LocalSinkState &lstate) const -> SinkResultType {
  // Append deep-copies, so the buffered rows never reference scan-owned buffers.
  static_cast<ParquetInsertLocalSinkState &>(lstate).rows_.Append(input);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalParquetInsert::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                    LocalSinkState &lstate) const {
  auto &gs = static_cast<ParquetInsertGlobalSinkState &>(gstate);
  auto &ls = static_cast<ParquetInsertLocalSinkState &>(lstate);
  std::lock_guard<std::mutex> guard(gs.lock_);
  gs.rows_.Append(ls.rows_);
}

auto PhysicalParquetInsert::Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t /*stage*/,
                                     idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<ParquetInsertGlobalSinkState &>(gstate);
  gs.count_ = gs.rows_.GetCount();
  if (gs.count_ == 0) {
    return SinkFinalizeType::READY;  // nothing to append, no manifest churn
  }

  auto *parquet = ResolveParquet(context, table_oid_);
  const auto table_name = TableName(context, table_oid_);
  const auto &dir = parquet->GetPath();

  // Lock BEFORE reading the manifest the append is based on.
  ExternalWriteGuard write_guard(*parquet, table_name);
  auto manifest = ReadManifestOrThrow(dir, table_name);

  auto entry = WritePartFile(dir, GeneratePartFileName(manifest.version_ + 1), *parquet->GetSchema(), gs.rows_);
  manifest.entries_.push_back(std::move(entry));
  manifest.version_ += 1;
  ParquetManifestIO::Write(dir, manifest);  // the commit point
  return SinkFinalizeType::READY;
}

auto PhysicalParquetInsert::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<ParquetWriteSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalParquetInsert::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                    LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<ParquetWriteSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;
  }
  const auto count = static_cast<int64_t>(static_cast<ParquetInsertGlobalSinkState *>(src.sink_)->count_);
  output.SetValue(0, 0, Value(count).CastAs(output_schema_->GetColumn(0).GetType()));
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalParquetInsert::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

auto PhysicalParquetInsert::ParamsToString() const -> std::string {
  return fmt::format("{{ table_oid={}, storage=parquet }}", table_oid_);
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

struct ParquetDeleteGlobalSinkState : GlobalSinkState {
  std::mutex lock_;
  std::vector<int64_t> rids_;
  idx_t count_{0};
};

struct ParquetDeleteLocalSinkState : LocalSinkState {
  std::vector<int64_t> rids_;
};

namespace {

/**
 * @brief The shared copy-on-write rewrite of DELETE and UPDATE.
 *
 * Streams every file that contains a touched row and lets `transform` decide per input chunk what
 * to append to the replacement file; untouched files carry forward into the new manifest as-is.
 * Commits by manifest swap, then unlinks the replaced originals.
 *
 * @param transform Called per (file_idx, chunk, first_row_index_in_file, writer).
 */
template <class TRANSFORM>
void RewriteTouchedFiles(ClientContext &context, table_oid_t table_oid, const std::unordered_set<idx_t> &touched_files,
                         const std::string &table_name, TRANSFORM &&transform) {
  auto *parquet = ResolveParquet(context, table_oid);
  const auto &dir = parquet->GetPath();
  const auto &schema = *parquet->GetSchema();

  ExternalWriteGuard write_guard(*parquet, table_name);
  auto manifest = ReadManifestOrThrow(dir, table_name);

  ParquetManifest next;
  next.version_ = manifest.version_ + 1;
  std::vector<std::string> replaced;

  for (idx_t file_idx = 0; file_idx < manifest.entries_.size(); file_idx++) {
    auto &entry = manifest.entries_[file_idx];
    if (!touched_files.contains(file_idx)) {
      next.entries_.push_back(entry);  // carried forward untouched
      continue;
    }

    // Stream the old file through the transform into a fresh part file.
    auto file_path = (fs::path(dir) / entry.file_name_).string();
    ParquetReader reader(GlobalParquetAllocator(), file_path);
    std::vector<idx_t> column_ids;
    for (idx_t c = 0; c < schema.GetColumnCount(); c++) {
      column_ids.push_back(c);
    }
    std::vector<idx_t> groups;
    for (idx_t g = 0; g < reader.NumRowGroups(); g++) {
      groups.push_back(g);
    }
    ParquetReaderScanState state;
    reader.InitializeScan(state, column_ids, groups);

    PartFileWriter writer(dir, GeneratePartFileName(next.version_), schema);
    DataChunk chunk;
    chunk.Initialize(reader.return_types_);
    idx_t row_offset = 0;
    reader.Scan(state, chunk);
    while (chunk.GetSize() > 0) {
      const idx_t n = chunk.GetSize();
      transform(file_idx, chunk, row_offset, writer);
      row_offset += n;
      chunk.Reset();
      reader.Scan(state, chunk);
    }
    auto new_entry = writer.Finish();
    replaced.push_back(entry.file_name_);
    if (new_entry.row_count_ > 0) {
      next.entries_.push_back(std::move(new_entry));
    } else {
      // Every row of this file was deleted: the empty replacement file is dropped outright.
      fs::remove(fs::path(dir) / new_entry.file_name_);
    }
  }

  ParquetManifestIO::Write(dir, next);  // the commit point

  // The old files are no longer referenced by any manifest; in-flight scans that opened them keep
  // their file handles (POSIX keeps unlinked files readable).
  for (const auto &name : replaced) {
    std::error_code ec;
    fs::remove(fs::path(dir) / name, ec);
  }
}

}  // namespace

auto PhysicalParquetDelete::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<ParquetDeleteGlobalSinkState>();
}

auto PhysicalParquetDelete::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<ParquetDeleteLocalSinkState>();
}

auto PhysicalParquetDelete::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                 LocalSinkState &lstate) const -> SinkResultType {
  CollectRids(input, rid_column_, static_cast<ParquetDeleteLocalSinkState &>(lstate).rids_);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalParquetDelete::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                    LocalSinkState &lstate) const {
  auto &gs = static_cast<ParquetDeleteGlobalSinkState &>(gstate);
  auto &ls = static_cast<ParquetDeleteLocalSinkState &>(lstate);
  std::lock_guard<std::mutex> guard(gs.lock_);
  gs.rids_.insert(gs.rids_.end(), ls.rids_.begin(), ls.rids_.end());
}

auto PhysicalParquetDelete::Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t /*stage*/,
                                     idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<ParquetDeleteGlobalSinkState &>(gstate);
  gs.count_ = gs.rids_.size();
  if (gs.count_ == 0) {
    return SinkFinalizeType::READY;
  }

  // Doomed row indices per file.
  std::unordered_map<idx_t, std::unordered_set<uint32_t>> doomed;
  std::unordered_set<idx_t> touched;
  for (auto rid : gs.rids_) {
    doomed[RidFileIdx(rid)].insert(RidRowIdx(rid));
    touched.insert(RidFileIdx(rid));
  }

  SelectionVector survivors(STANDARD_VECTOR_SIZE);
  RewriteTouchedFiles(context, table_oid_, touched, TableName(context, table_oid_),
                      [&](idx_t file_idx, DataChunk &chunk, idx_t row_offset, PartFileWriter &writer) {
                        auto &dead = doomed[file_idx];
                        idx_t n = 0;
                        for (idx_t i = 0; i < chunk.GetSize(); i++) {
                          if (!dead.contains(static_cast<uint32_t>(row_offset + i))) {
                            survivors.SetIndex(n++, i);
                          }
                        }
                        if (n == chunk.GetSize()) {
                          writer.Append(chunk);  // nothing doomed in this chunk
                          return;
                        }
                        chunk.Slice(survivors, n);
                        writer.Append(chunk);
                      });
  return SinkFinalizeType::READY;
}

auto PhysicalParquetDelete::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<ParquetWriteSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalParquetDelete::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                    LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<ParquetWriteSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;
  }
  const auto count = static_cast<int64_t>(static_cast<ParquetDeleteGlobalSinkState *>(src.sink_)->count_);
  output.SetValue(0, 0, Value(count).CastAs(output_schema_->GetColumn(0).GetType()));
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalParquetDelete::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

auto PhysicalParquetDelete::ParamsToString() const -> std::string {
  return fmt::format("{{ table_oid={}, storage=parquet }}", table_oid_);
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

/** One sink batch: the recomputed full-width rows and the RID each replaces. */
struct ParquetUpdateBatch {
  std::unique_ptr<DataChunk> new_rows_;
  std::vector<int64_t> rids_;
};

struct ParquetUpdateGlobalSinkState : GlobalSinkState {
  std::mutex lock_;
  std::vector<ParquetUpdateBatch> batches_;
  idx_t count_{0};
};

struct ParquetUpdateLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> executor_;
  std::vector<ParquetUpdateBatch> batches_;
};

auto PhysicalParquetUpdate::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<ParquetUpdateGlobalSinkState>();
}

auto PhysicalParquetUpdate::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<ParquetUpdateLocalSinkState>();
  ls->executor_ = std::make_unique<ExpressionExecutor>();
  for (const auto &e : target_expressions_) {
    ls->executor_->AddExpression(*e);
  }
  return ls;
}

auto PhysicalParquetUpdate::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                 LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<ParquetUpdateLocalSinkState &>(lstate);

  // Evaluate the SET expressions against the matched rows, producing the replacement rows.
  DataChunk computed;
  computed.Initialize(new_types_);
  ls.executor_->Execute(input, computed);

  ParquetUpdateBatch batch;
  batch.new_rows_ = std::make_unique<DataChunk>();
  batch.new_rows_->Initialize(new_types_);
  computed.Copy(*batch.new_rows_);  // own the values: `computed` may reference scan buffers
  CollectRids(input, rid_column_, batch.rids_);
  ls.batches_.push_back(std::move(batch));
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalParquetUpdate::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                    LocalSinkState &lstate) const {
  auto &gs = static_cast<ParquetUpdateGlobalSinkState &>(gstate);
  auto &ls = static_cast<ParquetUpdateLocalSinkState &>(lstate);
  std::lock_guard<std::mutex> guard(gs.lock_);
  for (auto &b : ls.batches_) {
    gs.batches_.push_back(std::move(b));
  }
  ls.batches_.clear();
}

auto PhysicalParquetUpdate::Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t /*stage*/,
                                     idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<ParquetUpdateGlobalSinkState &>(gstate);
  idx_t total = 0;
  for (const auto &b : gs.batches_) {
    total += b.rids_.size();
  }
  gs.count_ = total;
  if (total == 0) {
    return SinkFinalizeType::READY;
  }

  // Per file: row index -> (batch, row) of its replacement values.
  struct Replacement {
    const DataChunk *rows_;
    idx_t row_;
  };
  std::unordered_map<idx_t, std::unordered_map<uint32_t, Replacement>> replacements;
  std::unordered_set<idx_t> touched;
  for (const auto &b : gs.batches_) {
    for (idx_t i = 0; i < b.rids_.size(); i++) {
      const auto rid = b.rids_[i];
      replacements[RidFileIdx(rid)][RidRowIdx(rid)] = Replacement{b.new_rows_.get(), i};
      touched.insert(RidFileIdx(rid));
    }
  }

  RewriteTouchedFiles(context, table_oid_, touched, TableName(context, table_oid_),
                      [&](idx_t file_idx, DataChunk &chunk, idx_t row_offset, PartFileWriter &writer) {
                        auto &file_repl = replacements[file_idx];
                        // Substitute the replaced rows in place. Cell-wise Value copies are fine
                        // here: only the statement's matched rows take this path, the rest of the
                        // table streams through vectorized.
                        for (idx_t i = 0; i < chunk.GetSize(); i++) {
                          auto it = file_repl.find(static_cast<uint32_t>(row_offset + i));
                          if (it == file_repl.end()) {
                            continue;
                          }
                          for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
                            chunk.SetValue(c, i, it->second.rows_->GetValue(c, it->second.row_));
                          }
                        }
                        writer.Append(chunk);
                      });
  return SinkFinalizeType::READY;
}

auto PhysicalParquetUpdate::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<ParquetWriteSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalParquetUpdate::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                    LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<ParquetWriteSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;
  }
  const auto count = static_cast<int64_t>(static_cast<ParquetUpdateGlobalSinkState *>(src.sink_)->count_);
  output.SetValue(0, 0, Value(count).CastAs(output_schema_->GetColumn(0).GetType()));
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalParquetUpdate::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

auto PhysicalParquetUpdate::ParamsToString() const -> std::string {
  return fmt::format("{{ table_oid={}, storage=parquet }}", table_oid_);
}

}  // namespace bumblebee
