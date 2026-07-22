//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_hash_join.cpp
//
// Identification: src/execution/operator/join/physical_hash_join.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/join/physical_hash_join.h"

#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "catalog/schema.h"
#include "common/exception.h"
#include "execution/expression_executor.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/prl_hash_table.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"

namespace bumblebee {

namespace {

/** @brief A fresh (empty) join table for `op`: the build-key columns first, then the build columns. */
auto MakeJoinTable(const PhysicalHashJoin &op) -> std::unique_ptr<PRLHashTable> {
  std::vector<LogicalType> layout_types;
  for (const auto &k : op.BuildKeys()) {
    layout_types.push_back(k->GetReturnType().GetType());
  }
  const idx_t key_count = layout_types.size();
  for (const auto &col : op.children_[op.BuildChildIdx()]->output_schema_->GetColumns()) {
    layout_types.push_back(col.GetType());
  }
  return std::make_unique<PRLHashTable>(std::move(layout_types), key_count, /*null_equal_keys=*/false);
}

}  // namespace

struct HashJoinGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  // The join table (key columns first, then the build columns). Sink tasks scatter rows into their
  // thread-local tables in parallel; Combine splices them here block-wise; Finalize only builds the
  // directory over the already-materialized rows (the DuckDB build shape).
  std::unique_ptr<PRLHashTable> ht_;
};

struct HashJoinLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> key_exec_;  // build keys
  std::vector<LogicalType> key_types_;
  std::unique_ptr<PRLHashTable> ht_;  // unbuilt: rows + hashes only, no directory yet
};

struct HashJoinGlobalOperatorState : GlobalOperatorState {
  GlobalSinkState *sink_{nullptr};  // the same operator's sink state (its hash table)
};

struct HashJoinLocalOperatorState : LocalOperatorState {
  std::unique_ptr<ExpressionExecutor> key_exec_;  // probe keys
  std::vector<LogicalType> key_types_;
  bool have_matches_{false};
  // The joined pairs of the current probe chunk, in probe-row order. A null address is a LEFT probe
  // row that matched nothing: emitted once with the build columns NULL.
  std::vector<data_ptr_t> match_addrs_;
  std::vector<sel_t> match_rows_;
  idx_t cursor_{0};
};

auto PhysicalHashJoin::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<HashJoinGlobalSinkState>();
}

auto PhysicalHashJoin::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<HashJoinLocalSinkState>();
  ls->key_exec_ = std::make_unique<ExpressionExecutor>();
  for (const auto &k : BuildKeys()) {
    ls->key_exec_->AddExpression(*k);
    ls->key_types_.push_back(k->GetReturnType().GetType());
  }
  ls->ht_ = MakeJoinTable(*this);
  return ls;
}

auto PhysicalHashJoin::Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState & /*gstate*/,
                            LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<HashJoinLocalSinkState &>(lstate);
  // Reserve the build batch against the query budget (once per chunk). On overflow, bail with the logical
  // node so the driver re-lowers just this join to the grace hash join (which handles INNER and LEFT)
  // and retries — no spill code here, so the in-memory fast path is unchanged when the build fits.
  // EstimatedBytes counts string payloads too; the +16/row covers the row-layout bookkeeping.
  const idx_t bytes = input.EstimatedBytes() + input.GetSize() * 16;
  if (!context.client_.mem_.TryReserve(bytes)) {
    throw MemoryLimitException(logical_source_);
  }

  // Key, hash and scatter this batch into the thread-local table — the parallel half of the build.
  // A build row with a NULL key can never be matched, so it is left out of the table entirely.
  DataChunk key_chunk;
  key_chunk.Initialize(ls.key_types_);
  ls.key_exec_->Execute(input, key_chunk);

  DataChunk full;
  full.InitializeEmpty(ls.ht_->GetTypes());
  for (idx_t c = 0; c < ls.key_types_.size(); c++) {
    full.data_[c].Reference(key_chunk.data_[c]);
  }
  for (idx_t c = 0; c < input.ColumnCount(); c++) {
    full.data_[ls.key_types_.size() + c].Reference(input.data_[c]);
  }
  full.SetCardinality(input.GetSize());

  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, input.GetSize()};
  key_chunk.Hash(hashes);
  SelectionVector sel(input.GetSize());
  const idx_t n = PRLHashTable::NonNullKeyRows(key_chunk, sel);
  ls.ht_->AppendUnbuilt(hashes, full, sel, n);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalHashJoin::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &gs = static_cast<HashJoinGlobalSinkState &>(gstate);
  auto &ls = static_cast<HashJoinLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  if (gs.ht_ == nullptr) {
    gs.ht_ = std::move(ls.ht_);  // first task in: adopt its table wholesale
  } else {
    gs.ht_->Merge(*ls.ht_);  // block splice: O(blocks), no row ever re-scatters
  }
}

auto PhysicalHashJoin::Finalize(ClientContext & /*context*/, GlobalSinkState &gstate, idx_t /*stage*/,
                                idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<HashJoinGlobalSinkState &>(gstate);
  if (gs.ht_ == nullptr) {
    gs.ht_ = MakeJoinTable(*this);  // no sink task ran: an empty table, so the probe can assume one
  }

  // A LEFT join still emits every (preserved) probe row even against an empty build, so only INNER can
  // short-circuit the probe scan here.
  if (gs.ht_->Count() == 0) {
    return join_type_ == JoinType::INNER ? SinkFinalizeType::NO_OUTPUT_POSSIBLE : SinkFinalizeType::READY;
  }
  // The rows are already materialized (scattered in parallel by the sink tasks); all that remains is
  // one directory pass over the stored (hash, address) pairs.
  gs.ht_->BuildDirectory();
  return SinkFinalizeType::READY;
}

auto PhysicalHashJoin::GetGlobalOperatorState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalOperatorState> {
  auto gop = std::make_unique<HashJoinGlobalOperatorState>();
  gop->sink_ = own_sink_state;  // the probe reads the table its own sink built
  return gop;
}

auto PhysicalHashJoin::GetLocalOperatorState(ExecutionContext & /*context*/) const
    -> std::unique_ptr<LocalOperatorState> {
  auto ls = std::make_unique<HashJoinLocalOperatorState>();
  ls->key_exec_ = std::make_unique<ExpressionExecutor>();
  for (const auto &k : ProbeKeys()) {  // right keys for INNER, left keys for LEFT
    ls->key_exec_->AddExpression(*k);
    ls->key_types_.push_back(k->GetReturnType().GetType());
  }
  return ls;
}

namespace {

/**
 * @brief Key one probe chunk and collect its joined (build address, probe row) pairs in probe order.
 *
 * For LEFT the matches are merged with the unmatched probe rows, so every preserved row appears —
 * matched rows once per match, the rest (including NULL-keyed rows) once with a null address.
 */
void CollectMatches(PRLHashTable &ht, ExpressionExecutor &key_exec, const std::vector<LogicalType> &key_types,
                    DataChunk &input, bool left, std::vector<data_ptr_t> &out_addrs, std::vector<sel_t> &out_rows) {
  const idx_t count = input.GetSize();
  DataChunk key_chunk;
  key_chunk.Initialize(key_types);
  key_exec.Execute(input, key_chunk);
  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
  key_chunk.Hash(hashes);
  SelectionVector sel(count);
  const idx_t n_valid = PRLHashTable::NonNullKeyRows(key_chunk, sel);

  if (!left) {
    ht.Probe(hashes, key_chunk, sel, n_valid, out_addrs, out_rows);
    return;
  }
  std::vector<data_ptr_t> addrs;
  std::vector<sel_t> rows;
  std::vector<uint8_t> matched(count, 0);
  ht.Probe(hashes, key_chunk, sel, n_valid, addrs, rows, &matched);
  idx_t p = 0;
  for (idx_t i = 0; i < count; i++) {
    if (matched[i] != 0) {
      while (p < rows.size() && rows[p] == i) {
        out_addrs.push_back(addrs[p]);
        out_rows.push_back(rows[p]);
        p++;
      }
    } else {
      out_addrs.push_back(nullptr);
      out_rows.push_back(static_cast<sel_t>(i));
    }
  }
}

/**
 * @brief Emit one output batch of pairs: build columns gathered from the addresses (a null address
 * NULL-pads — the LEFT case), probe columns sliced zero-copy from the probe chunk.
 */
void EmitBatch(const PRLHashTable &ht, idx_t key_count, DataChunk &input, DataChunk &output,
               std::vector<data_ptr_t> &addrs, std::vector<sel_t> &rows, idx_t offset, idx_t n,
               idx_t build_cols, idx_t build_offset, idx_t probe_offset) {
  Vector addr_vec{LogicalType{LogicalTypeId::UBIGINT}, reinterpret_cast<data_ptr_t>(addrs.data() + offset)};
  SelectionVector probe_sel(rows.data() + offset);
  SelectionVector identity;
  for (idx_t c = 0; c < build_cols; c++) {
    RowOperations::Gather(ht.GetLayout(), addr_vec, identity, output.data_[build_offset + c], identity, n,
                          key_count + c);
  }
  output.Slice(input, probe_sel, n, probe_offset);
}

}  // namespace

auto PhysicalHashJoin::Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &output,
                               GlobalOperatorState &gstate, LocalOperatorState &lstate) const -> OperatorResultType {
  auto &gop = static_cast<HashJoinGlobalOperatorState &>(gstate);
  auto &ht = *static_cast<HashJoinGlobalSinkState *>(gop.sink_)->ht_;  // Finalize guarantees the table
  auto &ls = static_cast<HashJoinLocalOperatorState &>(lstate);
  const bool left = IsLeftJoin();

  if (!ls.have_matches_) {
    ls.match_addrs_.clear();
    ls.match_rows_.clear();
    ls.cursor_ = 0;
    CollectMatches(ht, *ls.key_exec_, ls.key_types_, input, left, ls.match_addrs_, ls.match_rows_);
    ls.have_matches_ = true;
  }

  // Output = left ++ right: for INNER the build side IS the left input (build columns first); for
  // LEFT the preserved probe side is the left input, the build columns follow.
  const idx_t total = ls.match_rows_.size();
  const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, total - ls.cursor_);
  if (n > 0) {
    const idx_t build_cols = left ? output.ColumnCount() - left_column_count_ : left_column_count_;
    EmitBatch(ht, BuildKeys().size(), input, output, ls.match_addrs_, ls.match_rows_, ls.cursor_, n, build_cols,
              /*build_offset=*/left ? left_column_count_ : 0, /*probe_offset=*/left ? 0 : build_cols);
  }
  output.SetCardinality(n);
  ls.cursor_ += n;

  if (ls.cursor_ >= total) {
    ls.have_matches_ = false;  // this probe chunk is fully joined; ask for the next
    return OperatorResultType::NEED_MORE_INPUT;
  }
  return OperatorResultType::HAVE_MORE_OUTPUT;  // more matches for the SAME input chunk
}

void PhysicalHashJoin::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  // sink + operator: the probe (the preserved side for LEFT) streams through this operator as part of
  // the current pipeline; the build side is its own child pipeline that must finish first.
  current.operators_.push_back(this);
  children_[ProbeChildIdx()]->BuildPipelines(current, builder);
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[BuildChildIdx()]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
