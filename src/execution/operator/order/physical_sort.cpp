//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_sort.cpp
//
// Identification: src/execution/operator/order/physical_sort.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/order/physical_sort.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "common/exception.h"
#include "execution/expression_executor.h"
#include "execution/sort/sorted_gather.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/string_heap.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

namespace {

auto TypesOf(const Schema &schema) -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(schema.GetColumnCount());
  for (const auto &col : schema.GetColumns()) {
    types.push_back(col.GetType());
  }
  return types;
}

}  // namespace

struct SortGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  ChunkCollection data_;         // the unsorted input; dropped once `sorted_data_` is built
  ChunkCollection sorted_data_;  // the output, materialized in sorted order at Finalize
};

struct SortLocalSinkState : LocalSinkState {
  ChunkCollection data_;
};

struct SortGlobalSourceState : GlobalSourceState {
  GlobalSinkState *sink_{nullptr};
  std::atomic<idx_t> cursor_{0};
  auto MaxThreads() -> idx_t override { return 1; }
};

auto PhysicalSort::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<SortGlobalSinkState>();
}

auto PhysicalSort::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<SortLocalSinkState>();
}

auto PhysicalSort::Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState & /*gstate*/,
                        LocalSinkState &lstate) const -> SinkResultType {
  // Reserve this batch against the query budget (once per chunk — negligible). On overflow, bail with
  // the logical node so the driver re-lowers just this sort to the external merge sort and retries.
  // EstimatedBytes counts string payloads too; the +16/row covers the sort entries built at Finalize.
  const idx_t bytes = input.EstimatedBytes() + input.GetSize() * 16;
  if (!context.client_.mem_.TryReserve(bytes)) {
    throw MemoryLimitException(logical_source_);
  }
  static_cast<SortLocalSinkState &>(lstate).data_.Append(input);  // copies the batch
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalSort::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &gs = static_cast<SortGlobalSinkState &>(gstate);
  auto &ls = static_cast<SortLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  gs.data_.Append(ls.data_);
}

auto PhysicalSort::Finalize(ClientContext & /*context*/, GlobalSinkState &gstate, idx_t /*stage*/, idx_t /*task_idx*/,
                            idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<SortGlobalSinkState &>(gstate);
  const idx_t n = gs.data_.GetCount();
  if (n == 0) {
    return SinkFinalizeType::READY;
  }

  // Encode every row's ORDER BY keys into a memcmp-comparable byte string (vectorized, one
  // CreateSortKey per chunk), then stable-sort the entries by it.
  ExpressionExecutor exec;
  std::vector<LogicalType> key_types;
  for (const auto &ob : order_bys_) {
    exec.AddExpression(*std::get<2>(ob));
    key_types.push_back(std::get<2>(ob)->GetReturnType().GetType());
  }

  StringHeap key_heap;  // owns the key bytes for the lifetime of the sort
  std::vector<SortEntry> entries;
  entries.reserve(n);
  DataChunk key_chunk;
  key_chunk.InitializeEmpty(key_types);
  idx_t global_row = 0;
  for (idx_t ci = 0; ci < gs.data_.ChunkCount(); ci++) {
    auto &chunk = gs.data_.GetChunk(ci);
    exec.Execute(chunk, key_chunk);
    Vector sort_keys{LogicalType{LogicalTypeId::STRING}};
    CreateSortKey::Create(key_chunk, modifiers_, sort_keys);
    const auto *keys = FlatVector::GetData<string_t>(sort_keys);
    for (idx_t i = 0; i < chunk.GetSize(); i++) {
      entries.push_back(SortEntry{key_heap.AddString(keys[i]), global_row++});
    }
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const SortEntry &a, const SortEntry &b) { return a.key_ < b.key_; });

  // Materialize the sorted order once, one vectorized gather per output chunk.
  const auto types = TypesOf(*output_schema_);
  for (idx_t pos = 0; pos < n; pos += STANDARD_VECTOR_SIZE) {
    const idx_t count = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - pos);
    auto out = std::make_unique<DataChunk>();
    out->Initialize(types);
    GatherSorted(gs.data_, entries.data() + pos, count, *out);
    out->SetCardinality(count);
    gs.sorted_data_.Append(std::move(out));
  }
  gs.data_.Reset();  // the unsorted copy is no longer needed
  return SinkFinalizeType::READY;
}

auto PhysicalSort::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<SortGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalSort::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                           LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<SortGlobalSourceState &>(gstate);
  auto &sink = *static_cast<SortGlobalSinkState *>(src.sink_);
  const idx_t total = sink.sorted_data_.GetCount();
  const idx_t start = src.cursor_.fetch_add(STANDARD_VECTOR_SIZE, std::memory_order_relaxed);
  if (start >= total) {
    return SourceResultType::FINISHED;
  }
  // Zero-copy: the sorted chunks are exactly VECTOR_SIZE-aligned, so a page is one whole chunk.
  auto &chunk = sink.sorted_data_.GetChunk(start / STANDARD_VECTOR_SIZE);
  output.Reference(chunk);
  return (start + chunk.GetSize() >= total) ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void PhysicalSort::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
