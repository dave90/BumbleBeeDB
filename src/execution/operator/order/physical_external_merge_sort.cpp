//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_external_merge_sort.cpp
//
// Identification: src/execution/operator/order/physical_external_merge_sort.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/order/physical_external_merge_sort.h"

#include <algorithm>
#include <memory>
#include <mutex>  // NOLINT
#include <queue>
#include <utility>
#include <vector>

#include "catalog/column.h"
#include "execution/expression_executor.h"
#include "execution/sort/sorted_gather.h"
#include "execution/spill/spill_collection.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/string_heap.h"
#include "type/vector/chunk_collection.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/** @brief Sort `batch` by its encoded ORDER BY key and write it, in order, as one `[key, cols...]` run. */
static void FlushRun(BufferPoolManager *bpm, const SchemaRef &run_schema, const std::vector<OrderModifiers> &mods,
                     ExpressionExecutor &key_exec, const std::vector<LogicalType> &key_types, ChunkCollection &batch,
                     std::vector<std::unique_ptr<SpillCollection>> &runs) {
  const idx_t n = batch.GetCount();
  if (n == 0) {
    return;
  }
  // Encode every row's ORDER BY keys (vectorized, one CreateSortKey per chunk), stable-sort the
  // lightweight entries by them.
  StringHeap key_heap;  // owns the key bytes until the run is written
  std::vector<SortEntry> entries;
  entries.reserve(n);
  DataChunk key_chunk;
  key_chunk.InitializeEmpty(key_types);
  idx_t global_row = 0;
  for (idx_t ci = 0; ci < batch.ChunkCount(); ci++) {
    auto &chunk = batch.GetChunk(ci);
    key_exec.Execute(chunk, key_chunk);
    Vector sort_keys{LogicalType{LogicalTypeId::STRING}};
    CreateSortKey::Create(key_chunk, mods, sort_keys);
    const auto *keys = FlatVector::GetData<string_t>(sort_keys);
    for (idx_t i = 0; i < chunk.GetSize(); i++) {
      entries.push_back(SortEntry{key_heap.AddString(keys[i]), global_row++});
    }
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const SortEntry &a, const SortEntry &b) { return a.key_ < b.key_; });

  // Write the run: per output chunk, fill the key column from the entries and gather the payload
  // columns with one batched selection copy per (source chunk, column).
  auto run = std::make_unique<SpillCollection>(bpm, run_schema);
  DataChunk out;
  out.Initialize(run_schema->GetTypes());
  for (idx_t pos = 0; pos < n; pos += STANDARD_VECTOR_SIZE) {
    const idx_t count = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - pos);
    out.Reset();
    auto *key_data = FlatVector::GetData<string_t>(out.data_[0]);
    for (idx_t i = 0; i < count; i++) {
      key_data[i] = StringVector::AddString(out.data_[0], entries[pos + i].key_);
    }
    GatherSorted(batch, entries.data() + pos, count, out, /*col_offset=*/1);
    out.SetCardinality(count);
    run->Append(out);
  }
  runs.push_back(std::move(run));
  batch.Reset();
}

struct RunCursor {
  std::unique_ptr<TableScan> scan_;
  std::unique_ptr<DataChunk> chunk_;
  idx_t pos_{0};
  bool done_{false};
};

/**
 * One run's head in the merge heap: its current key and the run it came from. The key points into
 * the run's resident chunk, which is safe because an entry is always popped before that chunk is
 * refilled — so the comparisons are plain memcmps, no per-row string allocation.
 */
struct MergeEntry {
  string_t key_;
  idx_t run_;
};

/** Orders the merge heap as a min-heap on the key (std::priority_queue is a max-heap). */
struct MergeEntryGreater {
  auto operator()(const MergeEntry &a, const MergeEntry &b) const -> bool { return b.key_ < a.key_; }
};

struct EmsGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  std::vector<std::unique_ptr<SpillCollection>> runs_;
};

struct EmsLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> key_exec_;
  std::vector<LogicalType> key_types_;
  ChunkCollection batch_;
  idx_t reserved_{0};
  std::vector<std::unique_ptr<SpillCollection>> runs_;
};

struct EmsGlobalSourceState : GlobalSourceState {
  GlobalSinkState *sink_{nullptr};
  bool initialized_{false};
  std::vector<RunCursor> cursors_;
  std::priority_queue<MergeEntry, std::vector<MergeEntry>, MergeEntryGreater> heap_;
  /** Per run: the (row in resident chunk, row in output) pairs not yet copied out. */
  std::vector<std::vector<sel_t>> pending_src_;
  std::vector<std::vector<sel_t>> pending_tgt_;
  auto MaxThreads() -> idx_t override { return 1; }
};

PhysicalExternalMergeSort::PhysicalExternalMergeSort(SchemaRef output_schema, std::vector<OrderBy> order_bys,
                                                     std::unique_ptr<PhysicalOperator> child)
    : PhysicalOperator(PhysicalOperatorType::EXTERNAL_MERGE_SORT, std::move(output_schema),
                       child->estimated_cardinality_),
      order_bys_(std::move(order_bys)) {
  for (const auto &ob : order_bys_) {
    modifiers_.emplace_back(std::get<0>(ob) == OrderByType::DESC ? OrderType::DESCENDING : OrderType::ASCENDING);
  }
  std::vector<Column> cols;
  cols.emplace_back("__key", LogicalType(LogicalTypeId::STRING), 1U << 20);
  for (const auto &c : output_schema_->GetColumns()) {
    cols.push_back(c);
  }
  run_schema_ = std::make_shared<const Schema>(cols);
  children_.push_back(std::move(child));
}

auto PhysicalExternalMergeSort::GetGlobalSinkState(ClientContext & /*context*/) const
    -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<EmsGlobalSinkState>();
}

auto PhysicalExternalMergeSort::GetLocalSinkState(ExecutionContext & /*context*/) const
    -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<EmsLocalSinkState>();
  ls->key_exec_ = std::make_unique<ExpressionExecutor>();
  for (const auto &ob : order_bys_) {
    ls->key_exec_->AddExpression(*std::get<2>(ob));
    ls->key_types_.push_back(std::get<2>(ob)->GetReturnType().GetType());
  }
  return ls;
}

auto PhysicalExternalMergeSort::Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState & /*gstate*/,
                                     LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<EmsLocalSinkState &>(lstate);
  auto &mem = context.client_.mem_;
  // EstimatedBytes counts string payloads too; the +16/row covers the run's encoded-key column.
  const idx_t bytes = input.EstimatedBytes() + input.GetSize() * 16;

  // If reserving this chunk would blow the budget and we already have a batch, spill the batch first.
  if (!mem.TryReserve(bytes) && ls.batch_.GetCount() > 0) {
    mem.Release(ls.reserved_);
    ls.reserved_ = 0;
    FlushRun(context.client_.bpm_, run_schema_, modifiers_, *ls.key_exec_, ls.key_types_, ls.batch_, ls.runs_);
    (void)mem.TryReserve(bytes);  // best effort for the incoming chunk
  }
  ls.reserved_ += bytes;
  ls.batch_.Append(input);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalExternalMergeSort::Combine(ExecutionContext &context, GlobalSinkState &gstate,
                                        LocalSinkState &lstate) const {
  auto &gs = static_cast<EmsGlobalSinkState &>(gstate);
  auto &ls = static_cast<EmsLocalSinkState &>(lstate);
  FlushRun(context.client_.bpm_, run_schema_, modifiers_, *ls.key_exec_, ls.key_types_, ls.batch_, ls.runs_);
  context.client_.mem_.Release(ls.reserved_);
  ls.reserved_ = 0;

  std::lock_guard lock(gs.mu_);
  for (auto &run : ls.runs_) {
    gs.runs_.push_back(std::move(run));
  }
}

auto PhysicalExternalMergeSort::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<EmsGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalExternalMergeSort::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                        LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<EmsGlobalSourceState &>(gstate);
  auto &sink = *static_cast<EmsGlobalSinkState *>(src.sink_);
  const auto run_types = run_schema_->GetTypes();

  // The current key of a run's cursor: a string_t into the resident chunk's key column (col 0).
  auto key_at = [](RunCursor &rc) -> string_t { return FlatVector::GetData<string_t>(rc.chunk_->data_[0])[rc.pos_]; };

  if (!src.initialized_) {
    // TODO(sort): multi-pass merge. This opens ALL runs at once — one resident chunk per run — so with
    // very many small runs the merge itself can exceed the memory budget. Cap the fan-in at MERGE_FANOUT
    // (see common/config.h) and merge in passes: repeatedly merge groups of <= MERGE_FANOUT runs into
    // fewer, larger runs until <= MERGE_FANOUT remain, then do the final merge here. This bounds resident
    // memory to MERGE_FANOUT chunks regardless of run count, at the cost of extra read/write passes.
    for (idx_t i = 0; i < sink.runs_.size(); i++) {
      RunCursor rc;
      rc.scan_ = sink.runs_[i]->MakeScan();
      rc.chunk_ = std::make_unique<DataChunk>();
      rc.chunk_->Initialize(run_types);
      if (!rc.scan_->Next(*rc.chunk_)) {
        rc.done_ = true;
      } else {
        rc.chunk_->Normalify();  // key_at and the batched copies read the columns flat
        src.heap_.push(MergeEntry{key_at(rc), i});
      }
      src.cursors_.push_back(std::move(rc));
    }
    src.pending_src_.resize(src.cursors_.size());
    src.pending_tgt_.resize(src.cursors_.size());
    src.initialized_ = true;
  }

  // The heap loop only decides the ORDER; the data movement is batched. A pop records a
  // (chunk row, output row) pair for its run, and the pairs are copied out with one selection
  // copy per (run, column) — either when that run's resident chunk is about to be refilled, or
  // at the end of the block.
  const idx_t ncols = output.ColumnCount();
  auto flush_pending = [&](idx_t ri) {
    auto &pending_src = src.pending_src_[ri];
    if (pending_src.empty()) {
      return;
    }
    auto &pending_tgt = src.pending_tgt_[ri];
    SelectionVector src_sel(pending_src.data());
    SelectionVector tgt_sel(pending_tgt.data());
    auto &chunk = *src.cursors_[ri].chunk_;
    for (idx_t c = 0; c < ncols; c++) {
      VectorOperations::Copy(chunk.data_[1 + c], output.data_[c], src_sel, &tgt_sel, pending_src.size(), 0, 0);
    }
    pending_src.clear();
    pending_tgt.clear();
  };

  idx_t filled = 0;
  while (filled < STANDARD_VECTOR_SIZE && !src.heap_.empty()) {
    const idx_t ri = src.heap_.top().run_;
    src.heap_.pop();
    auto &rc = src.cursors_[ri];
    src.pending_src_[ri].push_back(static_cast<sel_t>(rc.pos_));
    src.pending_tgt_[ri].push_back(static_cast<sel_t>(filled++));
    rc.pos_++;
    if (rc.pos_ >= rc.chunk_->GetSize()) {
      flush_pending(ri);  // the resident chunk is about to be overwritten
      if (!rc.scan_->Next(*rc.chunk_)) {
        rc.done_ = true;
      } else {
        rc.chunk_->Normalify();
        rc.pos_ = 0;
      }
    }
    if (!rc.done_) {
      src.heap_.push(MergeEntry{key_at(rc), ri});
    }
  }
  for (idx_t ri = 0; ri < src.cursors_.size(); ri++) {
    flush_pending(ri);
  }
  output.SetCardinality(filled);
  return filled == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void PhysicalExternalMergeSort::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
