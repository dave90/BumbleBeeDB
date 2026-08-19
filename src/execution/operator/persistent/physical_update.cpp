//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_update.cpp
//
// Identification: src/execution/operator/persistent/physical_update.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/persistent/physical_update.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <vector>

#include "catalog/catalog.h"
#include "execution/operator/resolve_table_storage.h"
#include "common/exception.h"
#include "execution/expression_executor.h"
#include "execution/operator/persistent/index_maintenance.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "storage/mvcc/mvcc.h"
#include "storage/table/table_heap.h"
#include "type/vector/selection_vector.h"
#include "type/value.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** One Sink call's worth of rows: the pre-image, the computed replacement, and their RIDs. */
struct UpdateBatch {
  std::unique_ptr<DataChunk> old_chunk_;  // pre-image rows (table columns + trailing RID)
  std::unique_ptr<DataChunk> new_chunk_;  // computed replacement rows (table columns)
  std::vector<int64_t> rids_;
};

/** Rows whose primary key CHANGED: their new values, and the RIDs of the old tuples to tombstone. */
struct ChangedRows {
  std::unique_ptr<DataChunk> new_chunk_;
  std::vector<int64_t> old_rids_;
};

struct UpdateGlobalSinkState : GlobalSinkState {
  std::atomic<idx_t> count_{0};
  std::mutex mu_;                     // guards `changed_`
  std::vector<ChangedRows> changed_;  // key-changed rows from all workers, applied statement-wide in Finalize
};

struct UpdateLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> executor_;
  std::vector<UpdateBatch> batches_;  // one per Sink call, processed at Combine
};

struct UpdateGlobalSourceState : GlobalSourceState {
  std::atomic<bool> emitted_{false};
  GlobalSinkState *sink_{nullptr};
};

auto PhysicalUpdate::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<UpdateGlobalSinkState>();
}

auto PhysicalUpdate::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<UpdateLocalSinkState>();
  ls->executor_ = std::make_unique<ExpressionExecutor>();
  for (const auto &e : target_expressions_) {
    ls->executor_->AddExpression(*e);
  }
  return ls;
}

auto PhysicalUpdate::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                          LocalSinkState &lstate) const -> SinkResultType {
  // The new values depend on the OLD row, so compute them now (while we have it) but DEEP-COPY them and
  // buffer the RIDs — the write itself waits for Combine, after the scan releases its page guards.
  auto &ls = static_cast<UpdateLocalSinkState &>(lstate);
  const idx_t count = input.GetSize();

  DataChunk computed;
  computed.Initialize(new_types_);
  ls.executor_->Execute(input, computed);

  UpdateBatch batch;
  batch.new_chunk_ = std::make_unique<DataChunk>();
  batch.new_chunk_->Initialize(new_types_);
  computed.Copy(*batch.new_chunk_);  // deep copy: `computed` references transient input columns

  // Keep an owned copy of the OLD row too (table columns + trailing RID), so Finalize can move each index
  // key from its old value to its new one. Deep copy: `input` is a transient (possibly filtered) slice.
  batch.old_chunk_ = std::make_unique<DataChunk>();
  batch.old_chunk_->Initialize(input.GetTypes());
  input.Copy(*batch.old_chunk_);

  input.data_[rid_column_].Normalify(count);
  const auto *rd = FlatVector::GetData<int64_t>(input.data_[rid_column_]);
  batch.rids_.assign(rd, rd + count);

  ls.batches_.push_back(std::move(batch));
  return SinkResultType::NEED_MORE_INPUT;
}

// A flat BIGINT RID vector filled from `src[0..n)`.
static auto MakeRidVector(const std::vector<int64_t> &src, idx_t n) -> Vector {
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  auto *out = FlatVector::GetData<int64_t>(rids);
  for (idx_t i = 0; i < n; i++) {
    out[i] = src[i];
  }
  return rids;
}

void PhysicalUpdate::Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &ls = static_cast<UpdateLocalSinkState &>(lstate);
  auto &gs = static_cast<UpdateGlobalSinkState &>(gstate);
  auto *heap = ResolveTableStorage<TableHeap>(context.client_, table_oid_, "Update", "a row-format heap");
  auto info = context.client_.catalog_.GetTable(table_oid_);
  auto pk = FindPrimaryKeyIndex(context.client_.catalog_, *info);

  idx_t total = 0;
  std::vector<ChangedRows> local_changed;
  for (auto &batch : ls.batches_) {
    const idx_t n = batch.rids_.size();
    total += n;
    batch.old_chunk_->Normalify();
    batch.new_chunk_->Normalify();

    if (pk == nullptr) {
      // No primary-key index (legacy table): a plain in-place update of every row.
      Vector rids = MakeRidVector(batch.rids_, n);
      MvccUpdate(&context.client_.txn_mgr_, context.client_.txn_, table_oid_, *heap, rids, *batch.new_chunk_);
      continue;
    }

    // Split the batch: rows whose PK is unchanged are updated in place now (parallel, no index change);
    // rows whose PK changed are deferred to Finalize, where the whole statement's moves are applied as a
    // tombstone-then-insert so any permutation across chunks/workers succeeds.
    auto changed = PrimaryKeyChanged(*pk, *batch.old_chunk_, *batch.new_chunk_);
    SelectionVector unchanged_sel(n == 0 ? 1 : n);
    SelectionVector changed_sel(n == 0 ? 1 : n);
    idx_t cu = 0;
    idx_t cc = 0;
    for (idx_t i = 0; i < n; i++) {
      (changed[i] ? changed_sel : unchanged_sel).SetIndex(changed[i] ? cc++ : cu++, i);
    }

    if (cu > 0) {
      DataChunk sub;
      sub.Initialize(new_types_);
      batch.new_chunk_->Copy(sub, unchanged_sel, cu);
      std::vector<int64_t> sub_rids(cu);
      for (idx_t i = 0; i < cu; i++) {
        sub_rids[i] = batch.rids_[unchanged_sel.GetIndex(i)];
      }
      Vector rids = MakeRidVector(sub_rids, cu);
      MvccUpdate(&context.client_.txn_mgr_, context.client_.txn_, table_oid_, *heap, rids, sub);
    }
    if (cc > 0) {
      ChangedRows cr;
      cr.new_chunk_ = std::make_unique<DataChunk>();
      cr.new_chunk_->Initialize(new_types_);
      batch.new_chunk_->Copy(*cr.new_chunk_, changed_sel, cc);
      cr.old_rids_.resize(cc);
      for (idx_t i = 0; i < cc; i++) {
        cr.old_rids_[i] = batch.rids_[changed_sel.GetIndex(i)];
      }
      local_changed.push_back(std::move(cr));
    }
  }

  if (!local_changed.empty()) {
    std::lock_guard lock(gs.mu_);
    for (auto &cr : local_changed) {
      gs.changed_.push_back(std::move(cr));
    }
  }
  gs.count_.fetch_add(total, std::memory_order_relaxed);
}

auto PhysicalUpdate::Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t /*stage*/, idx_t /*task_idx*/,
                              idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<UpdateGlobalSinkState &>(gstate);
  if (gs.changed_.empty()) {
    return SinkFinalizeType::READY;
  }
  auto info = context.catalog_.GetTable(table_oid_);
  auto pk = FindPrimaryKeyIndex(context.catalog_, *info);
  auto *heap = ResolveTableStorage<TableHeap>(context, table_oid_, "Update", "a row-format heap");

  // A key-changing update is a tombstone of the old tuple + an insert of the new key. Doing it in two
  // statement-wide phases — delete EVERY changed row's old tuple, then insert-or-revive every new row —
  // lets a permutation (swap / rotation) succeed: each new key's slot has already been freed.
  for (auto &cr : gs.changed_) {
    const idx_t n = cr.old_rids_.size();
    Vector rids = MakeRidVector(cr.old_rids_, n);
    MvccDelete(&context.txn_mgr_, context.txn_, table_oid_, *heap, rids, n);
  }
  if (pk != nullptr) {
    for (auto &cr : gs.changed_) {
      cr.new_chunk_->Normalify();
      Vector out_rids{LogicalType{LogicalTypeId::BIGINT}};
      InsertOrRevive(&context.txn_mgr_, context.txn_, table_oid_, *heap, *pk, *cr.new_chunk_, out_rids);
    }
  }
  return SinkFinalizeType::READY;
}

auto PhysicalUpdate::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<UpdateGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalUpdate::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                             LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<UpdateGlobalSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;
  }
  const auto count = static_cast<int64_t>(static_cast<UpdateGlobalSinkState *>(src.sink_)->count_.load());
  output.SetValue(0, 0, Value(count).CastAs(output_schema_->GetColumn(0).GetType()));
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalUpdate::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
