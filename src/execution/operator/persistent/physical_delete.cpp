//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_delete.cpp
//
// Identification: src/execution/operator/persistent/physical_delete.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/persistent/physical_delete.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "catalog/catalog.h"
#include "execution/operator/resolve_table_storage.h"
#include "common/exception.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "storage/mvcc/mvcc.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/vector.h"

namespace bumblebee {

struct DeleteGlobalSinkState : GlobalSinkState {
  std::atomic<idx_t> count_{0};
};

struct DeleteLocalSinkState : LocalSinkState {
  std::vector<int64_t> rids_;  // packed RIDs to delete, applied at Combine
};

struct DeleteGlobalSourceState : GlobalSourceState {
  std::atomic<bool> emitted_{false};
  GlobalSinkState *sink_{nullptr};
};

auto PhysicalDelete::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<DeleteGlobalSinkState>();
}

auto PhysicalDelete::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<DeleteLocalSinkState>();
}

auto PhysicalDelete::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                          LocalSinkState &lstate) const -> SinkResultType {
  // Only buffer the RIDs here — the scan still holds a read guard on the page these rows live on, so
  // modifying it now would self-deadlock. Combine applies the deletes once the scan has drained.
  auto &ls = static_cast<DeleteLocalSinkState &>(lstate);
  const idx_t count = input.GetSize();
  input.data_[rid_column_].Normalify(count);  // flatten a filtered slice to packed RIDs
  const auto *rd = FlatVector::GetData<int64_t>(input.data_[rid_column_]);
  ls.rids_.insert(ls.rids_.end(), rd, rd + count);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalDelete::Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &ls = static_cast<DeleteLocalSinkState &>(lstate);
  auto *heap = ResolveTableStorage<TableHeap>(context.client_, table_oid_, "Delete", "a row-format heap");
  const idx_t total = ls.rids_.size();
  for (idx_t off = 0; off < total; off += STANDARD_VECTOR_SIZE) {
    const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, total - off);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    auto *out = FlatVector::GetData<int64_t>(rids);
    for (idx_t i = 0; i < n; i++) {
      out[i] = ls.rids_[off + i];
    }
    MvccDelete(&context.client_.txn_mgr_, context.client_.txn_, table_oid_, *heap, rids, n);
  }

  // DELETE only tombstones the tuple; the primary-key index (a stable key -> RID directory) is left
  // untouched. The entry now points at a deleted tuple, so a later INSERT of that key sees "not live" and
  // revives the slot — and an abort un-tombstones the row without any index work.

  static_cast<DeleteGlobalSinkState &>(gstate).count_.fetch_add(total, std::memory_order_relaxed);
}

auto PhysicalDelete::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<DeleteGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalDelete::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                             LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<DeleteGlobalSourceState &>(gstate);
  if (src.emitted_.exchange(true, std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;
  }
  const auto count = static_cast<int64_t>(static_cast<DeleteGlobalSinkState *>(src.sink_)->count_.load());
  output.SetValue(0, 0, Value(count).CastAs(output_schema_->GetColumn(0).GetType()));
  output.SetCardinality(1);
  return SourceResultType::FINISHED;
}

void PhysicalDelete::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
