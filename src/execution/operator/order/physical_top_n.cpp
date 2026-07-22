//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_top_n.cpp
//
// Identification: src/execution/operator/order/physical_top_n.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/order/physical_top_n.h"

#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <vector>

#include "execution/expression_executor.h"
#include "execution/sort/top_n_heap.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"

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

struct TopNGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  std::unique_ptr<TopNHeap> heap_;
};

struct TopNLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> exec_;
  DataChunk key_chunk_;
  std::unique_ptr<TopNHeap> heap_;
};

struct TopNGlobalSourceState : GlobalSourceState {
  GlobalSinkState *sink_{nullptr};
  std::atomic<idx_t> cursor_{0};
  auto MaxThreads() -> idx_t override { return 1; }
};

auto PhysicalTopN::MakeHeap() const -> std::unique_ptr<TopNHeap> {
  std::vector<LogicalType> key_types;
  key_types.reserve(order_bys_.size());
  for (const auto &ob : order_bys_) {
    key_types.push_back(std::get<2>(ob)->GetReturnType().GetType());
  }
  return std::make_unique<TopNHeap>(TypesOf(*output_schema_), key_types, modifiers_, n_);
}

auto PhysicalTopN::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  auto gs = std::make_unique<TopNGlobalSinkState>();
  gs->heap_ = MakeHeap();
  return gs;
}

auto PhysicalTopN::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<TopNLocalSinkState>();
  ls->exec_ = std::make_unique<ExpressionExecutor>();
  std::vector<LogicalType> key_types;
  for (const auto &ob : order_bys_) {
    ls->exec_->AddExpression(*std::get<2>(ob));
    key_types.push_back(std::get<2>(ob)->GetReturnType().GetType());
  }
  ls->key_chunk_.InitializeEmpty(key_types);  // Execute() references, so no data is needed
  ls->heap_ = MakeHeap();
  return ls;
}

auto PhysicalTopN::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                        LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<TopNLocalSinkState &>(lstate);
  ls.exec_->Execute(input, ls.key_chunk_);
  ls.heap_->Sink(input, ls.key_chunk_);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalTopN::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &gs = static_cast<TopNGlobalSinkState &>(gstate);
  auto &ls = static_cast<TopNLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  gs.heap_->Combine(*ls.heap_);
}

auto PhysicalTopN::Finalize(ClientContext & /*context*/, GlobalSinkState &gstate, idx_t /*stage*/, idx_t /*task_idx*/,
                            idx_t /*task_count*/) const -> SinkFinalizeType {
  static_cast<TopNGlobalSinkState &>(gstate).heap_->Finalize();
  return SinkFinalizeType::READY;
}

auto PhysicalTopN::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<TopNGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalTopN::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                           LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<TopNGlobalSourceState &>(gstate);
  auto &heap = *static_cast<TopNGlobalSinkState *>(src.sink_)->heap_;
  const idx_t start = src.cursor_.fetch_add(STANDARD_VECTOR_SIZE, std::memory_order_relaxed);
  const idx_t count = heap.GetData(output, start);
  if (count == 0) {
    return SourceResultType::FINISHED;
  }
  return (start + count >= heap.GetSize()) ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void PhysicalTopN::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
