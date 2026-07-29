//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_table_scan.cpp
//
// Identification: src/execution/operator/scan/physical_table_scan.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/scan/physical_table_scan.h"

#include <algorithm>
#include <memory>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "fmt/format.h"
#include "parallel/pipeline_builder.h"
#include "storage/table/table_heap.h"
#include "type/value.h"

namespace bumblebee {

namespace {

/** @brief Resolve the row-format heap behind a table oid, or throw if it is missing / not a heap. */
auto ResolveHeap(ClientContext &context, table_oid_t oid) -> TableHeap * {
  auto info = context.catalog_.GetTable(oid);
  if (info == NULL_TABLE_INFO || info->storage_ == nullptr) {
    throw ExecutionException("TableScan: table has no storage backend");
  }
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  if (heap == nullptr) {
    throw ExecutionException("TableScan: table is not a row-format heap");
  }
  return heap;
}

}  // namespace

/** @brief The shared scan snapshot for a whole `PhysicalTableScan`. */
struct TableScanGlobalState : GlobalSourceState {
  std::shared_ptr<ParallelScanState> scan_state_;
  // One shared constant-NULL vector per pruned column: every emitted chunk references it instead
  // of allocating a fresh constant (immutable, so sharing across worker threads is safe).
  std::vector<std::unique_ptr<Vector>> null_vectors_;

  auto MaxThreads() -> idx_t override {
    const idx_t pages = scan_state_->NumPages();
    const idx_t morsel_pages = scan_state_->morsel_pages_;
    return std::max<idx_t>(1, (pages + morsel_pages - 1) / morsel_pages);
  }
};

/** @brief One worker's cursor over the morsel it is currently draining. */
struct TableScanLocalState : LocalSourceState {
  std::unique_ptr<TableScan> cursor_;
  // Projected scans gather into this narrow chunk (one vector per projected column); its vectors
  // are then referenced zero-copy into the full-width output slots.
  DataChunk narrow_;
  bool narrow_ready_{false};
  // The first pruned output column, or ColumnCount when none: its encoding tells whether the
  // shared constant-NULL references are still in place (see GetData).
  idx_t first_null_column_{0};
};

void PhysicalTableScan::BuildPipelines(Pipeline &current, PipelineBuilder & /*builder*/) const {
  current.source_ = this;  // a scan closes the pipeline: it is a source only
}

auto PhysicalTableScan::GetGlobalSourceState(ClientContext &context, GlobalSinkState * /*own*/) const
    -> std::unique_ptr<GlobalSourceState> {
  auto *heap = ResolveHeap(context, table_oid_);
  auto gs = std::make_unique<TableScanGlobalState>();
  gs->scan_state_ = heap->BeginParallelScan(&context.txn_mgr_, context.txn_, table_oid_, predicate_, projection_);
  // Seed the per-scan morsel granularity from the session config; a lowered value lets a small table
  // span several morsels so tests exercise the multi-morsel path without a large row count.
  gs->scan_state_->morsel_pages_ = std::max<idx_t>(1, context.config_.morsel_pages_);
  if (!projection_.empty()) {
    gs->null_vectors_.resize(output_schema_->GetColumnCount());
    for (idx_t c = 0, k = 0; c < output_schema_->GetColumnCount(); c++) {
      if (k < projection_.size() && projection_[k] == c) {
        k++;
        continue;
      }
      gs->null_vectors_[c] = std::make_unique<Vector>(Value::Null(output_schema_->GetColumn(c).GetType()));
    }
  }
  return gs;
}

auto PhysicalTableScan::GetLocalSourceState(ExecutionContext & /*context*/, GlobalSourceState & /*gstate*/) const
    -> std::unique_ptr<LocalSourceState> {
  return std::make_unique<TableScanLocalState>();
}

auto PhysicalTableScan::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                LocalSourceState &lstate) const -> SourceResultType {
  auto &gs = static_cast<TableScanGlobalState &>(gstate);
  auto &ls = static_cast<TableScanLocalState &>(lstate);
  auto &heap = *gs.scan_state_->heap_;

  // Projected scan: the heap gathers only the projected columns, into a chunk that narrow. The
  // full-width output then references those vectors in their original slots (zero-copy; validity
  // is shared) and constant-NULLs the pruned ones, so downstream column numbering is unchanged.
  const bool projected = !projection_.empty();
  DataChunk *scan_target = &output;
  if (projected) {
    BUMBLEBEE_ASSERT(!emit_rids_, "projected scans are never used for DML");
    if (!ls.narrow_ready_) {
      std::vector<LogicalType> types;
      types.reserve(projection_.size());
      for (auto col : projection_) {
        types.push_back(output_schema_->GetColumn(col).GetType());
      }
      ls.narrow_.Initialize(types);
      ls.narrow_ready_ = true;
      ls.first_null_column_ = output.ColumnCount();
      for (idx_t c = 0, k = 0; c < output.ColumnCount(); c++) {
        if (k < projection_.size() && projection_[k] == c) {
          k++;
          continue;
        }
        ls.first_null_column_ = c;
        break;
      }
    }
    scan_target = &ls.narrow_;
  }

  // When emitting RIDs, the trailing output column receives them; otherwise the scan produces no RIDs.
  Vector *rid_out = emit_rids_ ? &output.data_[output.ColumnCount() - 1] : nullptr;
  while (true) {
    if (ls.cursor_) {
      scan_target->Reset();
      if (ls.cursor_->Next(*scan_target, rid_out)) {
        if (projected) {
          for (idx_t k = 0; k < projection_.size(); k++) {
            output.data_[projection_[k]].Reference(ls.narrow_.data_[k]);
          }
         if (ls.first_null_column_ != output.ColumnCount() &&
              output.data_[ls.first_null_column_].GetVectorType() != VectorType::CONSTANT_VECTOR) {
            for (idx_t c = 0, k = 0; c < output.ColumnCount(); c++) {
              if (k < projection_.size() && projection_[k] == c) {
                k++;
                continue;
              }
              output.data_[c].Reference(*gs.null_vectors_[c]);
            }
          }
          output.SetCardinality(ls.narrow_.GetSize());
        }
        return SourceResultType::HAVE_MORE_OUTPUT;
      }
      ls.cursor_.reset();  // this morsel is drained; pull the next
    }
    idx_t begin;
    idx_t end;
    if (!gs.scan_state_->NextMorsel(begin, end)) {
      return SourceResultType::FINISHED;  // the morsel space is exhausted
    }
    ls.batch_idx_ = begin;  // page order == serial scan order, one morsel per task at a time
    ls.cursor_ = heap.MakeMorselScan(gs.scan_state_, begin, end);
  }
}

auto PhysicalTableScan::ParamsToString() const -> std::string {
  return fmt::format("{{ table={}, storage=row }}", table_name_);
}

}  // namespace bumblebee
