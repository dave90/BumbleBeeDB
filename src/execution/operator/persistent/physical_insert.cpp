//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_insert.cpp
//
// Identification: src/execution/operator/persistent/physical_insert.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/persistent/physical_insert.h"

#include <atomic>
#include <memory>
#include <vector>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "execution/operator/persistent/index_maintenance.h"
#include "fmt/format.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "storage/mvcc/mvcc.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/operations/vector_operations.h"
#include "type/vector/vector.h"

namespace bumblebee {

namespace {

auto ResolveHeap(ClientContext &context, table_oid_t oid) -> TableHeap * {
  auto info = context.catalog_.GetTable(oid);
  if (info == NULL_TABLE_INFO || info->storage_ == nullptr) {
    throw ExecutionException("Insert: table has no storage backend");
  }
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  if (heap == nullptr) {
    throw ExecutionException("Insert: table is not a row-format heap");
  }
  return heap;
}

auto SchemaTypes(const Schema &schema) -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(schema.GetColumnCount());
  for (const auto &c : schema.GetColumns()) {
    types.push_back(c.GetType());
  }
  return types;
}

}  // namespace

/** @brief The running count of inserted rows (a per-task atomic add). */
struct InsertGlobalSinkState : GlobalSinkState {
  std::atomic<idx_t> count_{0};
};

/** @brief One task's buffered input chunks, deep-copied; the heap writes wait for Combine. */
struct InsertLocalSinkState : LocalSinkState {
  std::vector<std::unique_ptr<DataChunk>> batches_;
};

/** @brief Source-side state: emit the count exactly once, reading it from the sink state. */
struct InsertGlobalSourceState : GlobalSourceState {
  std::atomic<bool> emitted_{false};
  GlobalSinkState *sink_{nullptr};
};

auto PhysicalInsert::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<InsertGlobalSinkState>();
}

auto PhysicalInsert::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<InsertLocalSinkState>();
}

auto PhysicalInsert::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                          LocalSinkState &lstate) const -> SinkResultType {
  // Only buffer a deep copy here — when the child pipeline scans the SAME table (INSERT ... SELECT
  // FROM itself), the scan still holds a read guard on the page the append would write-latch, so
  // inserting now would self-deadlock. Combine applies the inserts once the scan has drained (the
  // same deferral Update and Delete use).
  auto &ls = static_cast<InsertLocalSinkState &>(lstate);
  auto chunk = std::make_unique<DataChunk>();
  chunk->Initialize(input.GetTypes());
  input.Copy(*chunk);
  ls.batches_.push_back(std::move(chunk));
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalInsert::Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &ls = static_cast<InsertLocalSinkState &>(lstate);
  auto info = context.client_.catalog_.GetTable(table_oid_);
  auto *heap = ResolveHeap(context.client_, table_oid_);
  auto pk = FindPrimaryKeyIndex(context.client_.catalog_, *info);

  idx_t total = 0;
  for (auto &batch : ls.batches_) {
    const idx_t n = batch->GetSize();
    total += n;

    // Auto `_id` primary key: the VALUES supply the user columns; we prepend column 0 with the next block
    // of the per-table counter, producing the full-width row the heap expects.
    DataChunk widened;
    DataChunk *to_insert = batch.get();
    if (info->auto_id_) {
      const int64_t start = info->next_id_.fetch_add(static_cast<int64_t>(n));
      widened.Initialize(SchemaTypes(info->schema_));
      // Column 0 (`_id`, BIGINT): the contiguous sequence [start, start + n) written in one vectorized,
      // type-templated pass — a tight linear store, no per-row Value boxing.
      VectorOperations::GenerateSequence(widened.data_[0], n, start, /*increment=*/1);
      // Columns 1..k: the user columns referenced zero-copy from the buffered batch (no per-cell copy) —
      // the same vectors the non-auto-id path feeds straight to MvccInsert.
      for (idx_t c = 0; c < batch->ColumnCount(); c++) {
        widened.data_[c + 1].Reference(batch->data_[c]);
      }
      widened.SetCardinality(n);
      to_insert = &widened;
    }

    Vector rids{LogicalType{LogicalTypeId::BIGINT}};

    // The primary-key index is a stable key -> RID directory: route the insert through it so a key whose
    // tuple was deleted revives that slot, a live key is rejected as a duplicate, and a fresh key appends a
    // new tuple + index entry. A table with no PK index (legacy, direct-catalog) just appends.
    if (pk != nullptr) {
      InsertOrRevive(&context.client_.txn_mgr_, context.client_.txn_, table_oid_, *heap, *pk, *to_insert, rids);
    } else {
      MvccInsert(&context.client_.txn_mgr_, context.client_.txn_, table_oid_, *heap, *to_insert, rids);
    }
  }

  static_cast<InsertGlobalSinkState &>(gstate).count_.fetch_add(total, std::memory_order_relaxed);
}

auto PhysicalInsert::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<InsertGlobalSourceState>();
  src->sink_ = own_sink_state;  // the source reads the count its own sink accumulated
  return src;
}

auto PhysicalInsert::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                             LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<InsertGlobalSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;  // some other task already emitted the single count row
  }
  const auto count = static_cast<int64_t>(static_cast<InsertGlobalSinkState *>(src.sink_)->count_.load());
  output.SetValue(0, 0, Value(count).CastAs(output_schema_->GetColumn(0).GetType()));
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalInsert::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  // sink + source: the parent pipeline reads the count FROM this operator; a child pipeline sinks INTO it.
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

auto PhysicalInsert::ParamsToString() const -> std::string { return fmt::format("{{ table_oid={} }}", table_oid_); }

}  // namespace bumblebee
