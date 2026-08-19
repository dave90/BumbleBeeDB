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
#include "type/value.h"

namespace bumblebee {

/** @brief A fresh (empty) join table for `op`: the build-key columns first, then the LIVE build
 * columns (the pruning pass's annotation — dead build outputs are never stored or gathered). */
static auto MakeJoinTable(const PhysicalHashJoin &op) -> std::unique_ptr<PRLHashTable> {
  std::vector<LogicalType> layout_types;
  for (const auto &k : op.BuildKeys()) {
    layout_types.push_back(k->GetReturnType().GetType());
  }
  const idx_t key_count = layout_types.size();
  const auto &build_schema = *op.children_[op.BuildChildIdx()]->output_schema_;
  for (const auto c : op.live_build_columns_) {
    layout_types.push_back(build_schema.GetColumn(c).GetType());
  }
  return std::make_unique<PRLHashTable>(std::move(layout_types), key_count, /*null_equal_keys=*/false);
}

struct HashJoinGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  // The join table (key columns first, then the build columns). Sink tasks scatter rows into their
  // thread-local tables in parallel; Combine splices them here block-wise; Finalize only builds the
  // directory over the already-materialized rows (the DuckDB build shape).
  std::unique_ptr<PRLHashTable> ht_;
  // True if any build row carried a NULL key. Only read by a null-aware ANTI join (NOT IN), where
  // one NULL in the subquery result legally empties the whole output.
  bool build_has_null_key_{false};
};

struct HashJoinLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> key_exec_;  // build keys
  std::vector<LogicalType> key_types_;
  std::unique_ptr<PRLHashTable> ht_;  // unbuilt: rows + hashes only, no directory yet
  bool has_null_key_{false};
  // Per-chunk scratch, allocated once per task: Sink runs once per build chunk, and rebuilding
  // these there made every chunk pay a handful of mallocs (the key chunk's buffers are written
  // by Reference anyway, so they are allocated empty).
  DataChunk key_chunk_;
  DataChunk full_;
  Vector hashes_{LogicalType{LogicalTypeId::UBIGINT}, static_cast<idx_t>(STANDARD_VECTOR_SIZE)};
  SelectionVector sel_{static_cast<idx_t>(STANDARD_VECTOR_SIZE)};
};

struct HashJoinGlobalOperatorState : GlobalOperatorState {
  GlobalSinkState *sink_{nullptr};  // the same operator's sink state (its hash table)
  // The build columns NOT in the layout, and one shared constant-NULL vector per such column:
  // every emitted batch references those instead of gathering (nothing above reads them —
  // that is what made them dead). Immutable, so sharing across worker threads is safe.
  std::vector<idx_t> dead_build_columns_;
  std::vector<std::unique_ptr<Vector>> null_vectors_;  // indexed by build-child column
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
  // Per-chunk scratch, allocated once per task. The probe side of a big join runs this path tens of
  // thousands of times, so every buffer here used to be a malloc (and, for the zeroed ones, a
  // memset) per 1024 rows.
  DataChunk key_chunk_;
  Vector hashes_{LogicalType{LogicalTypeId::UBIGINT}, static_cast<idx_t>(STANDARD_VECTOR_SIZE)};
  SelectionVector valid_sel_{static_cast<idx_t>(STANDARD_VECTOR_SIZE)};
  SelectionVector out_sel_{static_cast<idx_t>(STANDARD_VECTOR_SIZE)};
  std::vector<uint8_t> matched_;
  std::vector<uint8_t> null_key_;
  std::vector<data_ptr_t> probe_addrs_;
  std::vector<sel_t> probe_rows_;
  PRLHashTable::ProbeState probe_state_;
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
  ls->key_chunk_.InitializeEmpty(ls->key_types_);
  ls->full_.InitializeEmpty(ls->ht_->GetTypes());
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
  auto &key_chunk = ls.key_chunk_;
  ls.key_exec_->Execute(input, key_chunk);

  auto &full = ls.full_;
  for (idx_t c = 0; c < ls.key_types_.size(); c++) {
    full.data_[c].Reference(key_chunk.data_[c]);
  }
  for (idx_t c = 0; c < live_build_columns_.size(); c++) {
    full.data_[ls.key_types_.size() + c].Reference(input.data_[live_build_columns_[c]]);
  }
  full.SetCardinality(input.GetSize());

  key_chunk.Hash(ls.hashes_);
  const idx_t n = PRLHashTable::NonNullKeyRows(key_chunk, ls.sel_);
  if (n < input.GetSize()) {
    ls.has_null_key_ = true;  // a dropped row had a NULL key (see build_has_null_key_)
  }
  ls.ht_->AppendUnbuilt(ls.hashes_, full, ls.sel_, n);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalHashJoin::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate, LocalSinkState &lstate) const {
  auto &gs = static_cast<HashJoinGlobalSinkState &>(gstate);
  auto &ls = static_cast<HashJoinLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  gs.build_has_null_key_ |= ls.has_null_key_;
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

  // NOT IN with a NULL anywhere in the subquery result: no probe row can ever prove non-membership,
  // so the whole output is legally empty — skip the probe outright.
  if (join_type_ == JoinType::ANTI && null_aware_ && gs.build_has_null_key_) {
    return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
  }

  // LEFT and ANTI still emit (preserved) probe rows against an empty build, so only INNER and SEMI
  // can short-circuit the probe scan here.
  if (gs.ht_->Count() == 0) {
    return join_type_ == JoinType::INNER || join_type_ == JoinType::SEMI ? SinkFinalizeType::NO_OUTPUT_POSSIBLE
                                                                         : SinkFinalizeType::READY;
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
  if (!IsSemiOrAnti()) {
    const auto &build_schema = *children_[BuildChildIdx()]->output_schema_;
    gop->null_vectors_.resize(build_schema.GetColumnCount());
    for (idx_t c = 0, k = 0; c < build_schema.GetColumnCount(); c++) {
      if (k < live_build_columns_.size() && live_build_columns_[k] == c) {
        k++;
        continue;
      }
      gop->dead_build_columns_.push_back(c);
      gop->null_vectors_[c] = std::make_unique<Vector>(Value::Null(build_schema.GetColumn(c).GetType()));
    }
  }
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
  ls->key_chunk_.InitializeEmpty(ls->key_types_);  // written by Reference, never allocated into
  return ls;
}

/**
 * @brief Key one probe chunk and collect its joined (build address, probe row) pairs in probe order.
 *
 * For LEFT the matches are merged with the unmatched probe rows, so every preserved row appears —
 * matched rows once per match, the rest (including NULL-keyed rows) once with a null address.
 */
static void CollectMatches(PRLHashTable &ht, HashJoinLocalOperatorState &ls, DataChunk &input, bool left,
                           std::vector<data_ptr_t> &out_addrs, std::vector<sel_t> &out_rows) {
  const idx_t count = input.GetSize();
  auto &key_chunk = ls.key_chunk_;
  ls.key_exec_->Execute(input, key_chunk);
  key_chunk.Hash(ls.hashes_);
  const idx_t n_valid = PRLHashTable::NonNullKeyRows(key_chunk, ls.valid_sel_);

  if (!left) {
    ht.Probe(ls.probe_state_, ls.hashes_, key_chunk, ls.valid_sel_, n_valid, out_addrs, out_rows);
    return;
  }
  auto &addrs = ls.probe_addrs_;
  auto &rows = ls.probe_rows_;
  addrs.clear();
  rows.clear();
  ls.matched_.assign(count, 0);
  ht.Probe(ls.probe_state_, ls.hashes_, key_chunk, ls.valid_sel_, n_valid, addrs, rows, &ls.matched_);
  idx_t p = 0;
  for (idx_t i = 0; i < count; i++) {
    if (ls.matched_[i] != 0) {
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
 * @brief Emit one output batch of pairs: the LIVE build columns gathered from the addresses (a
 * null address NULL-pads — the LEFT case), the dead ones referencing shared constant NULLs, and
 * the probe columns sliced zero-copy from the probe chunk.
 */
static void EmitBatch(const PRLHashTable &ht, idx_t key_count, const std::vector<idx_t> &live_cols,
                      const HashJoinGlobalOperatorState &gop, DataChunk &input, DataChunk &output,
                      std::vector<data_ptr_t> &addrs, std::vector<sel_t> &rows, idx_t offset, idx_t n,
                      idx_t build_offset, idx_t probe_offset) {
  Vector addr_vec{LogicalType{LogicalTypeId::UBIGINT}, reinterpret_cast<data_ptr_t>(addrs.data() + offset)};
  SelectionVector probe_sel(rows.data() + offset);
  SelectionVector identity;
  for (idx_t k = 0; k < live_cols.size(); k++) {
    RowOperations::Gather(ht.GetLayout(), addr_vec, identity, output.data_[build_offset + live_cols[k]], identity, n,
                          key_count + k);
  }
  for (const auto c : gop.dead_build_columns_) {
    output.data_[build_offset + c].Reference(*gop.null_vectors_[c]);
  }
  output.Slice(input, probe_sel, n, probe_offset);
}

auto PhysicalHashJoin::Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &output,
                               GlobalOperatorState &gstate, LocalOperatorState &lstate) const -> OperatorResultType {
  auto &gop = static_cast<HashJoinGlobalOperatorState &>(gstate);
  auto &ht = *static_cast<HashJoinGlobalSinkState *>(gop.sink_)->ht_;  // Finalize guarantees the table
  auto &ls = static_cast<HashJoinLocalOperatorState &>(lstate);
  const bool left = IsLeftJoin();

  // SEMI/ANTI: each probe row is emitted at most once (as-is, left columns only), so a chunk always
  // fits in one output batch — no cursor, no gather, just a zero-copy slice of the qualifying rows.
  if (IsSemiOrAnti()) {
    const idx_t count = input.GetSize();
    auto &key_chunk = ls.key_chunk_;
    ls.key_exec_->Execute(input, key_chunk);
    key_chunk.Hash(ls.hashes_);
    auto &valid_sel = ls.valid_sel_;
    const idx_t n_valid = PRLHashTable::NonNullKeyRows(key_chunk, valid_sel);

    auto &matched = ls.matched_;
    matched.assign(count, 0);
    if (ht.Count() > 0) {
      ls.probe_addrs_.clear();  // pair lists are unused: only the matched flags matter here
      ls.probe_rows_.clear();
      ht.Probe(ls.probe_state_, ls.hashes_, key_chunk, valid_sel, n_valid, ls.probe_addrs_, ls.probe_rows_, &matched);
    }

    // A NULL probe key never matched anything, so ANTI would emit it — correct for NOT EXISTS, but
    // IN's three-valued logic says `NULL NOT IN (non-empty)` is unknown, never true; null-aware
    // drops those rows. Against an EMPTY subquery result NOT IN is vacuously true for every row
    // (NULL included), so the drop only applies when the build side holds at least one row.
    auto &null_key = ls.null_key_;
    null_key.assign(count, 1);
    for (idx_t j = 0; j < n_valid; j++) {
      null_key[valid_sel.GetIndex(j)] = 0;
    }
    const bool drop_null_probe = null_aware_ && ht.Count() > 0;

    const bool anti = join_type_ == JoinType::ANTI;
    auto &out_sel = ls.out_sel_;
    idx_t n = 0;
    for (idx_t i = 0; i < count; i++) {
      const bool emit = anti ? (matched[i] == 0 && (!drop_null_probe || null_key[i] == 0)) : matched[i] != 0;
      if (emit) {
        out_sel.SetIndex(n++, i);
      }
    }
    output.Slice(input, out_sel, n, /*col_offset=*/0);
    output.SetCardinality(n);
    return OperatorResultType::NEED_MORE_INPUT;
  }

  if (!ls.have_matches_) {
    ls.match_addrs_.clear();
    ls.match_rows_.clear();
    ls.cursor_ = 0;
    CollectMatches(ht, ls, input, left, ls.match_addrs_, ls.match_rows_);
    ls.have_matches_ = true;
  }

  // Output = left ++ right: for INNER the build side IS the left input (build columns first); for
  // LEFT the preserved probe side is the left input, the build columns follow.
  const idx_t total = ls.match_rows_.size();
  const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, total - ls.cursor_);
  if (n > 0) {
    const idx_t build_cols = left ? output.ColumnCount() - left_column_count_ : left_column_count_;
    EmitBatch(ht, BuildKeys().size(), live_build_columns_, gop, input, output, ls.match_addrs_, ls.match_rows_,
              ls.cursor_, n,
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
