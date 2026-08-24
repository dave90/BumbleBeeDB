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
#include "type/value.h"
#include "type/vector/vector.h"

namespace bumblebee {

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
  if (info == nullptr) {
    throw ExecutionException("Insert: table disappeared during execution");
  }

  idx_t total = 0;
  for (auto &batch : ls.batches_) {
    total += InsertTableChunk(context.client_.catalog_, &context.client_.txn_mgr_, context.client_.txn_, *info,
                              *batch);
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
