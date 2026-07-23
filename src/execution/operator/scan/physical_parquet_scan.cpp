//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_parquet_scan.cpp
//
// Identification: src/execution/operator/scan/physical_parquet_scan.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/scan/physical_parquet_scan.h"

#include <atomic>
#include <set>
#include <filesystem>
#include <vector>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "fmt/format.h"
#include "parallel/pipeline_builder.h"
#include "storage/parquet/external_schema.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/parquet/parquet_reader.h"
#include "storage/parquet/parquet_zone_filter.h"
#include "storage/table/parquet_table.h"
#include "type/value.h"

namespace bumblebee {

namespace {

/** @brief Resolve the external parquet storage behind a table oid. */
auto ResolveParquet(ClientContext &context, table_oid_t oid) -> ParquetTable * {
  auto info = context.catalog_.GetTable(oid);
  if (info == NULL_TABLE_INFO || info->storage_ == nullptr) {
    throw ExecutionException("ParquetScan: table has no storage backend");
  }
  auto *parquet = dynamic_cast<ParquetTable *>(info->storage_.get());
  if (parquet == nullptr) {
    throw ExecutionException("ParquetScan: table is not an external parquet table");
  }
  return parquet;
}

/** One morsel: one row group of one live file. */
struct ParquetMorsel {
  idx_t file_idx_;        // index into the snapshot's file list (the RID high half)
  idx_t group_idx_;       // row group within the file
  idx_t file_row_start_;  // absolute row offset of this group within its file (RID low half base)
};

}  // namespace

/** @brief The manifest snapshot + flattened row-group morsel space, shared by all workers. */
struct ParquetScanGlobalState : GlobalSourceState {
  std::vector<std::unique_ptr<ParquetReader>> readers_;  // one per live file, footer parsed once
  std::vector<ParquetMorsel> morsels_;
  std::atomic<idx_t> next_morsel_{0};
  std::vector<idx_t> column_ids_;  // file column per output column (identity + optional RID tail)
  std::vector<idx_t> null_columns_;  // pruned output columns, constant-NULLed per emitted chunk

  auto MaxThreads() -> idx_t override { return std::max<idx_t>(1, morsels_.size()); }
};

/** @brief One worker's cursor over the row group it is currently draining. */
struct ParquetScanLocalState : LocalSourceState {
  std::unique_ptr<ParquetReaderScanState> state_;
  idx_t file_idx_{0};
  idx_t next_rid_base_{0};  // absolute row offset in the file of the next chunk's first row
};

void PhysicalParquetScan::BuildPipelines(Pipeline &current, PipelineBuilder & /*builder*/) const {
  current.source_ = this;  // a scan closes the pipeline: it is a source only
}

auto PhysicalParquetScan::GetGlobalSourceState(ClientContext &context, GlobalSinkState * /*own*/) const
    -> std::unique_ptr<GlobalSourceState> {
  auto *parquet = ResolveParquet(context, table_oid_);
  const auto &dir = parquet->GetPath();
  const auto &schema = *parquet->GetSchema();

  auto gs = std::make_unique<ParquetScanGlobalState>();

  // Zone pruning: provable `col <op> const` conjuncts of the WHERE, checked against each row
  // group's min/max statistics. The streaming filter above the scan still applies the full
  // predicate, so pruning is pure optimization.
  std::vector<ZonePredicate> zone_predicates;
  ExtractZonePredicates(predicate_, zone_predicates);

  // The statement-level snapshot: the newest manifest, read exactly once per scan.
  auto manifest = ParquetManifestIO::ReadLatest(dir);
  if (!manifest.has_value()) {
    throw ExecutionException(
        fmt::format("external table '{}': no manifest found at '{}' (was the folder cleared?)", table_name_, dir));
  }

  for (idx_t file_idx = 0; file_idx < manifest->entries_.size(); file_idx++) {
    const auto &entry = manifest->entries_[file_idx];
    auto file_path = (std::filesystem::path(dir) / entry.file_name_).string();
    auto reader = std::make_unique<ParquetReader>(GlobalParquetAllocator(), file_path);
    // Guard against externally replaced files: the footer must still match the catalog schema.
    if (!ExternalSchemaMatches(schema, reader->names_, reader->return_types_)) {
      throw ExecutionException(fmt::format(
          "external table '{}': parquet file '{}' no longer matches the table schema", table_name_,
          entry.file_name_));
    }
    // Flatten this file's row groups into the morsel space, skipping groups whose statistics
    // prove no row can satisfy the predicate.
    idx_t row_start = 0;
    auto *metadata = reader->GetFileMetadata();
    for (idx_t g = 0; g < metadata->row_groups.size(); g++) {
      if (zone_predicates.empty() ||
          RowGroupCanMatch(metadata->row_groups[g], reader->return_types_, zone_predicates)) {
        gs->morsels_.push_back(ParquetMorsel{file_idx, g, row_start});
      }
      row_start += metadata->row_groups[g].num_rows;
    }
    gs->readers_.push_back(std::move(reader));
  }

  // Output columns map 1:1 onto file columns (validated above); a trailing RID column, when
  // present, has no file column behind it. Pruned columns get the same "no file column" marker,
  // so the reader never touches their pages at all.
  std::set<idx_t> wanted(projection_.begin(), projection_.end());
  for (idx_t i = 0; i < schema.GetColumnCount(); i++) {
    const bool pruned = !projection_.empty() && !wanted.contains(i);
    gs->column_ids_.push_back(pruned ? COLUMN_IDENTIFIER_ROW_ID : i);
    if (pruned) {
      gs->null_columns_.push_back(i);
    }
  }
  if (emit_rids_) {
    gs->column_ids_.push_back(COLUMN_IDENTIFIER_ROW_ID);
  }
  return gs;
}

auto PhysicalParquetScan::GetLocalSourceState(ExecutionContext & /*context*/, GlobalSourceState & /*gstate*/) const
    -> std::unique_ptr<LocalSourceState> {
  return std::make_unique<ParquetScanLocalState>();
}

auto PhysicalParquetScan::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                  LocalSourceState &lstate) const -> SourceResultType {
  auto &gs = static_cast<ParquetScanGlobalState &>(gstate);
  auto &ls = static_cast<ParquetScanLocalState &>(lstate);

  while (true) {
    if (ls.state_) {
      gs.readers_[ls.file_idx_]->Scan(*ls.state_, output);
      if (output.GetSize() > 0) {
        // Pruned columns were never decoded: surface them as constant NULLs (which by
        // construction of the pruning pass nothing ever reads) instead of stale buffers.
        for (auto c : gs.null_columns_) {
          output.data_[c].Reference(Value::Null(output_schema_->GetColumn(c).GetType()));
        }
        if (emit_rids_) {
          // rid = (file_index << 32) | row_index_in_file, for rows [next_rid_base_, +size).
          auto *rids = FlatVector::GetData<int64_t>(output.data_[output.ColumnCount() - 1]);
          const auto base = (static_cast<uint64_t>(ls.file_idx_) << 32U) | ls.next_rid_base_;
          for (idx_t i = 0; i < output.GetSize(); i++) {
            rids[i] = static_cast<int64_t>(base + i);
          }
          ls.next_rid_base_ += output.GetSize();
        }
        return SourceResultType::HAVE_MORE_OUTPUT;
      }
      ls.state_.reset();  // this row group is drained; pull the next
    }
    const auto morsel_idx = gs.next_morsel_.fetch_add(1);
    if (morsel_idx >= gs.morsels_.size()) {
      return SourceResultType::FINISHED;  // the morsel space is exhausted
    }
    const auto &morsel = gs.morsels_[morsel_idx];
    ls.file_idx_ = morsel.file_idx_;
    ls.next_rid_base_ = morsel.file_row_start_;
    ls.state_ = std::make_unique<ParquetReaderScanState>();
    gs.readers_[morsel.file_idx_]->InitializeScan(*ls.state_, gs.column_ids_, {morsel.group_idx_});
  }
}

auto PhysicalParquetScan::ParamsToString() const -> std::string {
  return fmt::format("{{ table={}, storage=parquet }}", table_name_);
}

}  // namespace bumblebee
