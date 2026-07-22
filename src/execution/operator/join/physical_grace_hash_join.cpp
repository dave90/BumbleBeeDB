//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_grace_hash_join.cpp
//
// Identification: src/execution/operator/join/physical_grace_hash_join.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/join/physical_grace_hash_join.h"

#include <algorithm>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "common/helper.h"
#include "execution/expression_executor.h"
#include "execution/prl_hash_table.h"
#include "execution/spill/spill_collection.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

namespace {

static_assert((GH_PARTITION_COUNT & (GH_PARTITION_COUNT - 1)) == 0, "GH_PARTITION_COUNT must be a power of two");
constexpr idx_t PARTITION_BITS = 6;  // log2(64); each recursion level consumes the next 6 hash bits

auto TypesOf(const Schema &schema) -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(schema.GetColumnCount());
  for (const auto &c : schema.GetColumns()) {
    types.push_back(c.GetType());
  }
  return types;
}

/**
 * @brief Which of the `GH_PARTITION_COUNT` buckets a key hash routes to at recursion `depth`.
 *
 * Each level uses the next 6 bits of the 64-bit key hash, so a partition pair that overflows can be
 * split again with fresh bits — UNLESS every row shares one key (a hot key), which maps to a single
 * bucket at every level and so never splits (→ the NLJ fallback).
 */
auto PartitionAt(hash_t hash, idx_t depth) -> idx_t {
  return (hash >> (depth * PARTITION_BITS)) & (GH_PARTITION_COUNT - 1);
}

void BuildKeyExec(const std::vector<AbstractExpressionRef> &keys, ExpressionExecutor &exec,
                  std::vector<LogicalType> &types) {
  for (const auto &k : keys) {
    exec.AddExpression(*k);
    types.push_back(k->GetReturnType().GetType());
  }
}

/** @brief Lazily create the 64 spill partitions of one side. */
void EnsurePartitions(std::vector<std::unique_ptr<SpillCollection>> &parts, BufferPoolManager *bpm,
                      const SchemaRef &schema) {
  if (parts.empty()) {
    for (idx_t p = 0; p < GH_PARTITION_COUNT; p++) {
      parts.push_back(std::make_unique<SpillCollection>(bpm, schema));
    }
  }
}

/**
 * @brief Route a chunk into `parts` by the key-hash bits of recursion `depth`.
 *
 * Chunk-at-a-time: one vectorized key + hash pass, then one zero-copy slice + spill append per
 * non-empty partition. When `filter_null_keys` is set, NULL-keyed rows are dropped — or diverted to
 * `null_sink` when given (LEFT probe side: those rows are preserved, just never joinable).
 */
void AppendPartitioned(DataChunk &chunk, ExpressionExecutor &key_exec, const std::vector<LogicalType> &key_types,
                       const std::vector<LogicalType> &row_types, idx_t depth, bool filter_null_keys,
                       std::vector<std::unique_ptr<SpillCollection>> &parts, SpillCollection *null_sink = nullptr) {
  const idx_t count = chunk.GetSize();
  DataChunk key_chunk;
  key_chunk.Initialize(key_types);
  key_exec.Execute(chunk, key_chunk);
  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
  key_chunk.Hash(hashes);
  hashes.Normalify(count);
  auto hash_data = FlatVector::GetData<hash_t>(hashes);

  SelectionVector valid(count);
  idx_t n_valid = count;
  if (filter_null_keys) {
    n_valid = PRLHashTable::NonNullKeyRows(key_chunk, valid);
    if (null_sink != nullptr && n_valid < count) {
      // The complement of `valid`, in row order.
      SelectionVector invalid(count);
      idx_t n_invalid = 0;
      idx_t v = 0;
      for (idx_t i = 0; i < count; i++) {
        if (v < n_valid && valid.GetIndex(v) == i) {
          v++;
        } else {
          invalid.SetIndex(n_invalid++, i);
        }
      }
      DataChunk nulls;
      nulls.InitializeEmpty(row_types);
      nulls.Slice(chunk, invalid, n_invalid);
      null_sink->Append(nulls);
    }
  }

  std::vector<std::vector<sel_t>> by_part(GH_PARTITION_COUNT);
  for (idx_t k = 0; k < n_valid; k++) {
    const idx_t i = filter_null_keys ? valid.GetIndex(k) : k;
    by_part[PartitionAt(hash_data[i], depth)].push_back(static_cast<sel_t>(i));
  }
  for (idx_t p = 0; p < GH_PARTITION_COUNT; p++) {
    if (by_part[p].empty()) {
      continue;
    }
    SelectionVector sel(by_part[p].data());
    DataChunk part;
    part.InitializeEmpty(row_types);
    part.Slice(chunk, sel, by_part[p].size());
    parts[p]->Append(part);
  }
}

/**
 * @brief Append one batch of joined rows to the staging collection.
 *
 * Build columns are gathered from the table rows at `build_offset`; probe columns are sliced from the
 * probe chunk at `probe_offset` (INNER puts the build/left first, LEFT the probe/left first). The
 * append deep-copies, so nothing staged outlives its source.
 */
void StageMatches(const std::vector<LogicalType> &out_types, DataChunk &probe_chunk, const SelectionVector &probe_sel,
                  const PRLHashTable &ht, const std::vector<data_ptr_t> &addrs, idx_t offset, idx_t n,
                  idx_t build_cols, idx_t build_offset, idx_t probe_offset, ChunkCollection &staging) {
  DataChunk stage;
  stage.Initialize(out_types);
  Vector addr_vec{LogicalType{LogicalTypeId::UBIGINT},
                  reinterpret_cast<data_ptr_t>(const_cast<data_ptr_t *>(addrs.data() + offset))};
  SelectionVector identity;
  const auto &layout = ht.GetLayout();
  const idx_t key_count = layout.GetColumnCount() - build_cols;
  for (idx_t c = 0; c < build_cols; c++) {
    RowOperations::Gather(layout, addr_vec, identity, stage.data_[build_offset + c], identity, n, key_count + c);
  }
  stage.Slice(probe_chunk, probe_sel, n, probe_offset);
  staging.Append(stage);
}

/**
 * @brief Where each side lands in the output: `left ++ right`, so INNER puts the build (left input)
 * columns first while LEFT puts the preserved probe (left input) columns first.
 */
struct OutputLayout {
  std::vector<LogicalType> types_;
  bool left_;           // LEFT OUTER join?
  idx_t build_cols_;    // how many output columns come from the build side
  idx_t build_offset_;  // where the build columns land
  idx_t probe_offset_;  // where the probe columns land
};

/**
 * @brief LEFT: stage probe rows with the build columns NULL — the unmatched rows of a chunk (when
 * `matched` is given), or the whole chunk (the NULL-keyed spill, which never joins by definition).
 */
void StageNullPadded(const OutputLayout &out, DataChunk &probe_chunk, const std::vector<uint8_t> *matched,
                     ChunkCollection &staging) {
  const idx_t count = probe_chunk.GetSize();
  std::vector<sel_t> rows;
  for (idx_t i = 0; i < count; i++) {
    if (matched == nullptr || (*matched)[i] == 0) {
      rows.push_back(static_cast<sel_t>(i));
    }
  }
  if (rows.empty()) {
    return;
  }
  DataChunk stage;
  stage.InitializeEmpty(out.types_);
  SelectionVector sel(rows.data());
  stage.Slice(probe_chunk, sel, rows.size(), out.probe_offset_);
  for (idx_t c = 0; c < out.build_cols_; c++) {
    stage.data_[out.build_offset_ + c].Reference(Value::Null(out.types_[out.build_offset_ + c]));
  }
  stage.SetCardinality(rows.size());
  staging.Append(stage);  // deep copy — the local selection may die after this
}

}  // namespace

PhysicalGraceHashJoin::PhysicalGraceHashJoin(SchemaRef output_schema, std::vector<AbstractExpressionRef> left_keys,
                                             std::vector<AbstractExpressionRef> right_keys, JoinType join_type,
                                             idx_t left_column_count, std::unique_ptr<PhysicalOperator> left,
                                             std::unique_ptr<PhysicalOperator> right)
    : PhysicalOperator(PhysicalOperatorType::GRACE_HASH_JOIN, std::move(output_schema),
                       (join_type == JoinType::LEFT ? left : right)->estimated_cardinality_),
      left_keys_(std::move(left_keys)),
      right_keys_(std::move(right_keys)),
      join_type_(join_type),
      left_column_count_(left_column_count) {
  key_modifiers_.assign(left_keys_.size(), OrderModifiers(OrderType::ASCENDING));
  children_.push_back(std::move(left));   // child 0 = left input
  children_.push_back(std::move(right));  // child 1 = right input
  build_schema_ = children_[BuildChildIdx()]->output_schema_;
  probe_schema_ = children_[ProbeChildIdx()]->output_schema_;
}

// --------------------------------------------------------------------------------------------------
// Sink: two partitioning passes through one state, ordered by the pipeline DAG.
// --------------------------------------------------------------------------------------------------

struct GraceGlobalSinkState : GlobalSinkState {
  /** Which pass is running. Flipped by Finalize, which the scheduler serializes between pipelines. */
  enum class Phase { PARTITION_BUILD, PARTITION_PROBE, DONE };

  Phase phase_{Phase::PARTITION_BUILD};
  // The shared partitions, created up front. Sink tasks append to them CONCURRENTLY: the routing
  // work (keys, hashes, slices) is task-local, and SpillCollection::Append synchronizes internally.
  std::vector<std::unique_ptr<SpillCollection>> build_parts_;
  std::vector<std::unique_ptr<SpillCollection>> probe_parts_;
  std::unique_ptr<SpillCollection> null_probe_rows_;  // LEFT: NULL-keyed preserved rows (never joinable)
  idx_t build_count_{0};  // summed at the first Finalize, once the build tasks are done
};

struct GraceLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> build_exec_;
  std::unique_ptr<ExpressionExecutor> probe_exec_;
  std::vector<LogicalType> build_key_types_;
  std::vector<LogicalType> probe_key_types_;
};

auto PhysicalGraceHashJoin::GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> {
  auto gs = std::make_unique<GraceGlobalSinkState>();
  EnsurePartitions(gs->build_parts_, context.bpm_, build_schema_);
  EnsurePartitions(gs->probe_parts_, context.bpm_, probe_schema_);
  if (IsLeftJoin()) {
    gs->null_probe_rows_ = std::make_unique<SpillCollection>(context.bpm_, probe_schema_);
  }
  return gs;
}

auto PhysicalGraceHashJoin::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<GraceLocalSinkState>();
  ls->build_exec_ = std::make_unique<ExpressionExecutor>();
  BuildKeyExec(BuildKeys(), *ls->build_exec_, ls->build_key_types_);
  ls->probe_exec_ = std::make_unique<ExpressionExecutor>();
  BuildKeyExec(ProbeKeys(), *ls->probe_exec_, ls->probe_key_types_);
  return ls;
}

auto PhysicalGraceHashJoin::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState &gstate,
                                 LocalSinkState &lstate) const -> SinkResultType {
  auto &gs = static_cast<GraceGlobalSinkState &>(gstate);
  auto &ls = static_cast<GraceLocalSinkState &>(lstate);

  if (gs.phase_ == GraceGlobalSinkState::Phase::PARTITION_BUILD) {
    // A NULL build key can never equi-match: dropped for good.
    AppendPartitioned(input, *ls.build_exec_, ls.build_key_types_, TypesOf(*build_schema_), 0,
                      /*filter_null_keys=*/true, gs.build_parts_);
  } else {
    // A NULL probe key never joins either — but a LEFT join must still preserve the row, so it is
    // diverted to the dedicated spill and emitted NULL-padded by the source.
    AppendPartitioned(input, *ls.probe_exec_, ls.probe_key_types_, TypesOf(*probe_schema_), 0,
                      /*filter_null_keys=*/true, gs.probe_parts_, gs.null_probe_rows_.get());
  }
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalGraceHashJoin::Combine(ExecutionContext & /*context*/, GlobalSinkState & /*gstate*/,
                                    LocalSinkState & /*lstate*/) const {
  // Nothing to merge: the sink tasks routed their chunks straight into the shared partitions
  // (SpillCollection::Append is concurrency-safe); the local state holds only the key executors.
}

auto PhysicalGraceHashJoin::Finalize(ClientContext & /*context*/, GlobalSinkState &gstate, idx_t /*stage*/,
                                     idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<GraceGlobalSinkState &>(gstate);
  if (gs.phase_ == GraceGlobalSinkState::Phase::PARTITION_BUILD) {
    gs.phase_ = GraceGlobalSinkState::Phase::PARTITION_PROBE;
    for (const auto &part : gs.build_parts_) {
      gs.build_count_ += part->Count();
    }
    // INNER against an empty build produces nothing: suppress the dependents' sources — the probe is
    // never even scanned. (A LEFT join still preserves every probe row, so it must keep going.)
    if (join_type_ == JoinType::INNER && gs.build_count_ == 0) {
      return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
    }
    return SinkFinalizeType::READY;
  }
  gs.phase_ = GraceGlobalSinkState::Phase::DONE;
  return SinkFinalizeType::READY;
}

// --------------------------------------------------------------------------------------------------
// Source: join the partition pairs, one resident build table at a time.
// --------------------------------------------------------------------------------------------------

namespace {

/** @brief One unit of join work: a build partition and the probe partition with the same hash bits. */
struct PartitionPair {
  SpillCollection *build_{nullptr};  // may be null/empty
  SpillCollection *probe_{nullptr};  // never null (empty probes are not enqueued)
  idx_t depth_{0};
};

}  // namespace

/**
 * @brief The shared join-phase state: a work queue of partition pairs claimed by parallel tasks.
 *
 * Pairs are fully independent (hash partitioning puts a key's rows in exactly one pair), so each
 * task joins its claimed pair end to end without touching another task's state. The one mutex only
 * guards claiming work, publishing recursion sub-pairs, and the one-time seeding.
 */
struct GraceGlobalSourceState : GlobalSourceState {
  GraceGlobalSinkState *sink_{nullptr};
  BufferPoolManager *bpm_{nullptr};
  idx_t total_budget_rows_{0};  // the whole query's resident-row allowance
  idx_t budget_rows_{0};        // one task's share (set at seeding: total / task fan-out)

  // Read-only after GetGlobalSourceState — shared by every task without locking.
  std::vector<LogicalType> build_key_types_;
  std::vector<LogicalType> probe_key_types_;
  std::vector<LogicalType> build_types_;
  std::vector<LogicalType> probe_types_;

  std::mutex mu_;  // guards everything below
  bool seeded_{false};  // the work list is seeded on the FIRST GetData call: the source state is
                        // created at executor initialization, before the sink pipelines have run
  bool null_rows_claimed_{false};                             // LEFT: one task drains the NULL spill
  std::vector<PartitionPair> work_;                           // pairs still to join (LIFO)
  std::vector<std::unique_ptr<SpillCollection>> sub_spills_;  // owns every recursion sub-partition
  idx_t task_count_{1};                                       // recorded by MaxThreads for the budget split

  auto MaxThreads() -> idx_t override {
    // Called when this pipeline's tasks are created — after both partitioning pipelines finalized,
    // so the fan-out can be sized from the actual work: one task per non-empty probe partition
    // (plus one for the LEFT NULL spill). Extra tasks beyond the scheduler's workers just queue.
    idx_t pairs = 0;
    for (const auto &part : sink_->probe_parts_) {
      if (part != nullptr && part->Count() > 0) {
        pairs++;
      }
    }
    if (sink_->null_probe_rows_ != nullptr) {
      pairs++;
    }
    task_count_ = std::max<idx_t>(1, pairs);
    return task_count_;
  }
};

/** @brief One task's private join state: the pair it is draining and its staged output. */
struct GraceLocalSourceState : LocalSourceState {
  std::unique_ptr<ExpressionExecutor> build_exec_;
  std::unique_ptr<ExpressionExecutor> probe_exec_;

  // The claimed pair: its resident build table (HT mode) or its build spill (NLJ mode), and a
  // streaming scan over its probe partition.
  bool pair_open_{false};
  bool pair_is_nlj_{false};
  std::unique_ptr<PRLHashTable> ht_;
  SpillCollection *nlj_build_{nullptr};
  SpillCollection *pair_probe_{nullptr};  // freed eagerly once this pair is drained
  std::unique_ptr<TableScan> probe_scan_;

  // LEFT: set on the one task that claimed the NULL-keyed preserved rows.
  std::unique_ptr<TableScan> null_scan_;

  // The staged output of the current step, emitted one chunk per GetData call.
  ChunkCollection staging_;
  idx_t staging_cursor_{0};
};

namespace {

/** @brief Load one build partition into a fresh resident table: build keys first, build columns after. */
auto LoadBuildTable(const GraceGlobalSourceState &src, GraceLocalSourceState &ls, SpillCollection *spill)
    -> std::unique_ptr<PRLHashTable> {
  auto layout_types = src.build_key_types_;
  for (const auto &t : src.build_types_) {
    layout_types.push_back(t);
  }
  const idx_t count = spill != nullptr ? spill->Count() : 0;
  auto ht = std::make_unique<PRLHashTable>(layout_types, src.build_key_types_.size(),
                                           /*null_equal_keys=*/false, count * 2);
  if (spill == nullptr || count == 0) {
    return ht;  // an empty table: every probe row of this pair misses (the LEFT empty-build case)
  }
  auto scan = spill->MakeScan();
  DataChunk chunk;
  chunk.Initialize(src.build_types_);
  SelectionVector identity;
  while (scan->Next(chunk)) {
    DataChunk kc;
    kc.Initialize(src.build_key_types_);
    ls.build_exec_->Execute(chunk, kc);
    DataChunk full;
    full.InitializeEmpty(layout_types);
    for (idx_t c = 0; c < src.build_key_types_.size(); c++) {
      full.data_[c].Reference(kc.data_[c]);
    }
    for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
      full.data_[src.build_key_types_.size() + c].Reference(chunk.data_[c]);
    }
    full.SetCardinality(chunk.GetSize());
    Vector h{LogicalType{LogicalTypeId::UBIGINT}, chunk.GetSize()};
    kc.Hash(h);
    ht->AppendUnbuilt(h, full, identity, chunk.GetSize());
    chunk.Reset();
  }
  ht->BuildDirectory();  // one sized-once pass over the whole partition
  return ht;
}

/** @brief HT mode: join one probe chunk against the resident build table and stage the output. */
void JoinProbeChunkHashed(const GraceGlobalSourceState &src, GraceLocalSourceState &ls, DataChunk &probe_chunk,
                          const OutputLayout &out) {
  const idx_t count = probe_chunk.GetSize();
  DataChunk keys;
  keys.Initialize(src.probe_key_types_);
  ls.probe_exec_->Execute(probe_chunk, keys);
  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
  keys.Hash(hashes);

  std::vector<uint8_t> matched(out.left_ ? count : 0, 0);
  std::vector<data_ptr_t> addrs;
  std::vector<sel_t> rows;
  SelectionVector identity;
  ls.ht_->Probe(hashes, keys, identity, count, addrs, rows, out.left_ ? &matched : nullptr);

  for (idx_t off = 0; off < rows.size(); off += STANDARD_VECTOR_SIZE) {
    const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, rows.size() - off);
    SelectionVector probe_sel(rows.data() + off);
    StageMatches(out.types_, probe_chunk, probe_sel, *ls.ht_, addrs, off, n, out.build_cols_, out.build_offset_,
                 out.probe_offset_, ls.staging_);
  }
  if (out.left_) {
    StageNullPadded(out, probe_chunk, &matched, ls.staging_);
  }
}

/**
 * @brief NLJ mode (hot key): join one probe chunk by flipping the roles.
 *
 * Index this chunk's probe rows (keyed on the probe keys, payload = the probe row number), then
 * stream the oversized build partition one chunk at a time, probing each build row into that table —
 * memory stays bounded by one chunk plus the tiny table.
 */
void JoinProbeChunkNestedLoop(const GraceGlobalSourceState &src, GraceLocalSourceState &ls, DataChunk &probe_chunk,
                              const OutputLayout &out) {
  const idx_t count = probe_chunk.GetSize();
  DataChunk keys;
  keys.Initialize(src.probe_key_types_);
  ls.probe_exec_->Execute(probe_chunk, keys);
  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
  keys.Hash(hashes);
  std::vector<uint8_t> matched(out.left_ ? count : 0, 0);
  SelectionVector identity;

  auto probe_types = src.probe_key_types_;
  probe_types.emplace_back(LogicalTypeId::BIGINT);  // the probe row number
  PRLHashTable probe_ht(probe_types, src.probe_key_types_.size(), /*null_equal_keys=*/false, count * 2);
  const idx_t row_no_offset = probe_ht.GetLayout().GetOffsets().back();
  {
    DataChunk full;
    full.InitializeEmpty(probe_types);
    for (idx_t c = 0; c < src.probe_key_types_.size(); c++) {
      full.data_[c].Reference(keys.data_[c]);
    }
    Vector row_numbers{LogicalType{LogicalTypeId::BIGINT}, count};
    auto rn = FlatVector::GetData<int64_t>(row_numbers);
    for (idx_t i = 0; i < count; i++) {
      rn[i] = static_cast<int64_t>(i);
    }
    full.data_[src.probe_key_types_.size()].Reference(row_numbers);
    full.SetCardinality(count);
    probe_ht.Append(hashes, full, identity, count);
  }

  auto scan = ls.nlj_build_->MakeScan();
  DataChunk chunk;
  chunk.Initialize(src.build_types_);
  while (scan->Next(chunk)) {
    DataChunk kc;
    kc.Initialize(src.build_key_types_);
    ls.build_exec_->Execute(chunk, kc);
    Vector h{LogicalType{LogicalTypeId::UBIGINT}, chunk.GetSize()};
    kc.Hash(h);
    std::vector<data_ptr_t> addrs;  // probe-table rows: hold the probe row number
    std::vector<sel_t> build_rows;  // the matching build row within this streamed chunk
    probe_ht.Probe(h, kc, identity, chunk.GetSize(), addrs, build_rows);

    for (idx_t off = 0; off < build_rows.size(); off += STANDARD_VECTOR_SIZE) {
      const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, build_rows.size() - off);
      // Build columns are sliced from the streamed chunk, probe columns from the probe chunk through
      // the row numbers recovered from the probe table.
      std::vector<sel_t> probe_rows(n);
      for (idx_t j = 0; j < n; j++) {
        probe_rows[j] = static_cast<sel_t>(Load<int64_t>(addrs[off + j] + row_no_offset));
        if (out.left_) {
          matched[probe_rows[j]] = 1;
        }
      }
      DataChunk stage;
      stage.InitializeEmpty(out.types_);
      SelectionVector build_sel(build_rows.data() + off);
      SelectionVector probe_sel(probe_rows.data());
      stage.Slice(chunk, build_sel, n, out.build_offset_);
      stage.Slice(probe_chunk, probe_sel, n, out.probe_offset_);
      stage.SetCardinality(n);
      ls.staging_.Append(stage);
    }
    chunk.Reset();
  }
  if (out.left_) {
    StageNullPadded(out, probe_chunk, &matched, ls.staging_);
  }
}

/**
 * @brief Open a claimed partition pair: make it resident (HT), mark it NLJ, or split it into 64
 * sub-pairs with the next hash bits (in which case nothing opens and the caller claims again).
 *
 * The splitting work (scans, spill writes) runs OUTSIDE the queue mutex — only publishing the
 * finished sub-pairs takes it, which is also the happens-before edge that lets another task safely
 * read spills this task just wrote.
 */
void OpenPair(GraceGlobalSourceState &src, GraceLocalSourceState &ls, const PartitionPair &pair) {
  const idx_t build_count = pair.build_ != nullptr ? pair.build_->Count() : 0;
  if (build_count <= src.budget_rows_) {
    ls.ht_ = LoadBuildTable(src, ls, pair.build_);
    ls.pair_is_nlj_ = false;
    if (pair.build_ != nullptr) {
      pair.build_->Free();  // fully copied into the resident table — the spill is dead
    }
  } else if (pair.depth_ < GH_MAX_RECURSION) {
    // Re-partition the BUILD side with the next 6 bits; if that fails to shrink it (a single hot key
    // never splits), nested-loop the original pair instead of re-scanning it again and again.
    std::vector<std::unique_ptr<SpillCollection>> sub_build;
    for (idx_t p = 0; p < GH_PARTITION_COUNT; p++) {
      sub_build.push_back(std::make_unique<SpillCollection>(src.bpm_, pair.build_->GetSchema()));
    }
    auto scan = pair.build_->MakeScan();
    DataChunk chunk;
    chunk.Initialize(src.build_types_);
    while (scan->Next(chunk)) {
      AppendPartitioned(chunk, *ls.build_exec_, src.build_key_types_, src.build_types_, pair.depth_ + 1,
                        /*filter_null_keys=*/false, sub_build);
      chunk.Reset();
    }
    idx_t max_sub = 0;
    for (const auto &sub : sub_build) {
      max_sub = std::max(max_sub, sub->Count());
    }
    if (max_sub < build_count) {
      // The split worked: re-partition the probe side the same way and publish the sub-pairs.
      std::vector<std::unique_ptr<SpillCollection>> sub_probe;
      for (idx_t p = 0; p < GH_PARTITION_COUNT; p++) {
        sub_probe.push_back(std::make_unique<SpillCollection>(src.bpm_, pair.probe_->GetSchema()));
      }
      auto probe_scan = pair.probe_->MakeScan();
      DataChunk pchunk;
      pchunk.Initialize(src.probe_types_);
      while (probe_scan->Next(pchunk)) {
        AppendPartitioned(pchunk, *ls.probe_exec_, src.probe_key_types_, src.probe_types_, pair.depth_ + 1,
                          /*filter_null_keys=*/false, sub_probe);
        pchunk.Reset();
      }
      {
        std::lock_guard lock(src.mu_);
        for (idx_t p = 0; p < GH_PARTITION_COUNT; p++) {
          if (sub_probe[p]->Count() == 0) {
            continue;  // no probe rows -> no output, INNER or LEFT alike
          }
          src.work_.push_back(PartitionPair{sub_build[p].get(), sub_probe[p].get(), pair.depth_ + 1});
          src.sub_spills_.push_back(std::move(sub_build[p]));
          src.sub_spills_.push_back(std::move(sub_probe[p]));
        }
      }
      // The parent pair's rows now live in the sub-pairs (and the sub-spills that were not enqueued
      // free themselves when the local vectors die): reclaim the parent's pages eagerly.
      pair.build_->Free();
      pair.probe_->Free();
      return;  // nothing opened; the caller claims the next pair
    }
    ls.nlj_build_ = pair.build_;
    ls.pair_is_nlj_ = true;
  } else {
    ls.nlj_build_ = pair.build_;
    ls.pair_is_nlj_ = true;
  }
  ls.probe_scan_ = pair.probe_->MakeScan();
  ls.pair_probe_ = pair.probe_;
  ls.pair_open_ = true;
}

// --- the GetData state machine, one step per function --------------------------------------------

/** @brief Emit the next staged chunk into `output`, if any is pending. */
auto EmitStaged(GraceLocalSourceState &ls, DataChunk &output) -> bool {
  if (ls.staging_cursor_ < ls.staging_.ChunkCount()) {
    auto &chunk = ls.staging_.GetChunk(ls.staging_cursor_++);
    chunk.Copy(output);
    output.SetCardinality(chunk.GetSize());
    return true;
  }
  ls.staging_.Reset();
  ls.staging_cursor_ = 0;
  return false;
}

/** @brief LEFT: stage the next chunk of the claimed NULL-keyed spill (freed once drained). */
auto DrainNullRows(GraceGlobalSinkState &gs, GraceGlobalSourceState &src, GraceLocalSourceState &ls,
                   const OutputLayout &out) -> bool {
  if (ls.null_scan_ == nullptr) {
    return false;
  }
  DataChunk chunk;
  chunk.Initialize(src.probe_types_);
  if (ls.null_scan_->Next(chunk)) {
    StageNullPadded(out, chunk, nullptr, ls.staging_);
    return true;
  }
  ls.null_scan_.reset();  // close the scan BEFORE freeing the pages it pinned
  gs.null_probe_rows_->Free();
  return true;
}

/** @brief Advance the claimed pair by one probe chunk; close it (and free its spills) when drained. */
auto AdvancePair(const GraceGlobalSourceState &src, GraceLocalSourceState &ls, const OutputLayout &out) -> bool {
  if (!ls.pair_open_) {
    return false;
  }
  DataChunk chunk;
  chunk.Initialize(src.probe_types_);
  if (ls.probe_scan_->Next(chunk)) {
    if (ls.pair_is_nlj_) {
      JoinProbeChunkNestedLoop(src, ls, chunk, out);
    } else {
      JoinProbeChunkHashed(src, ls, chunk, out);
    }
    return true;
  }
  // This pair is drained: its spills are dead — return their pages to the pool right now.
  ls.pair_open_ = false;
  ls.ht_.reset();
  ls.probe_scan_.reset();  // close the scan BEFORE freeing the pages it pinned
  ls.pair_probe_->Free();
  ls.pair_probe_ = nullptr;
  if (ls.nlj_build_ != nullptr) {
    ls.nlj_build_->Free();  // the HT case freed its build spill at load time already
    ls.nlj_build_ = nullptr;
  }
  return true;
}

/** What the shared queue handed this task: the NULL-rows spill, a partition pair, or nothing left. */
enum class Claim { NULL_ROWS, PAIR, NOTHING };

/**
 * @brief Claim the next unit of work under the queue mutex — the only step tasks share.
 *
 * Seeds the work list on first entry (deferred to here: the partitions only exist once both sink
 * pipelines have finished, which is guaranteed before the first GetData). A pair with no probe rows
 * produces nothing, INNER or LEFT alike; a pair with probe rows but no build side still matters to
 * LEFT (all its rows NULL-pad). The per-task resident budget is the total divided by the task
 * fan-out — every task may hold one build table at once.
 */
auto ClaimWork(GraceGlobalSinkState &gs, GraceGlobalSourceState &src, GraceLocalSourceState &ls, bool left,
               PartitionPair &pair) -> Claim {
  std::lock_guard lock(src.mu_);
  if (!src.seeded_) {
    src.seeded_ = true;
    src.budget_rows_ =
        std::max<idx_t>(STANDARD_VECTOR_SIZE, src.total_budget_rows_ / std::max<idx_t>(1, src.task_count_));
    for (idx_t p = 0; p < GH_PARTITION_COUNT; p++) {
      SpillCollection *probe = p < gs.probe_parts_.size() ? gs.probe_parts_[p].get() : nullptr;
      if (probe == nullptr || probe->Count() == 0) {
        continue;
      }
      SpillCollection *build = p < gs.build_parts_.size() ? gs.build_parts_[p].get() : nullptr;
      src.work_.push_back(PartitionPair{build, probe, 0});
    }
  }
  if (left && !src.null_rows_claimed_ && gs.null_probe_rows_ != nullptr) {
    src.null_rows_claimed_ = true;
    ls.null_scan_ = gs.null_probe_rows_->MakeScan();
    return Claim::NULL_ROWS;
  }
  if (src.work_.empty()) {
    return Claim::NOTHING;
  }
  pair = src.work_.back();
  src.work_.pop_back();
  return Claim::PAIR;
}

}  // namespace

auto PhysicalGraceHashJoin::GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<GraceGlobalSourceState>();
  src->sink_ = static_cast<GraceGlobalSinkState *>(own_sink_state);
  src->bpm_ = context.bpm_;
  std::vector<LogicalType> ignored;
  ExpressionExecutor throwaway;  // the types are wanted here; each task builds its own executors
  BuildKeyExec(BuildKeys(), throwaway, src->build_key_types_);
  BuildKeyExec(ProbeKeys(), throwaway, src->probe_key_types_);
  src->build_types_ = TypesOf(*build_schema_);
  src->probe_types_ = TypesOf(*probe_schema_);
  // Plan-time width, so no data to measure: the inline stride per column, plus an assumed average
  // out-of-line payload for variable-size types (strings, lists), plus 16 bytes of row bookkeeping.
  idx_t row_bytes = 16;
  for (const auto &type : src->build_types_) {
    const auto ptype = type.GetPhysicalType();
    row_bytes += LogicalType::SizeOf(ptype) + (LogicalType::IsConstantSize(ptype) ? 0 : 32);
  }
  src->total_budget_rows_ = std::max<idx_t>(STANDARD_VECTOR_SIZE, context.mem_.Budget() / row_bytes);
  src->budget_rows_ = src->total_budget_rows_;
  return src;
}

auto PhysicalGraceHashJoin::GetLocalSourceState(ExecutionContext & /*context*/, GlobalSourceState & /*gstate*/) const
    -> std::unique_ptr<LocalSourceState> {
  auto ls = std::make_unique<GraceLocalSourceState>();
  ls->build_exec_ = std::make_unique<ExpressionExecutor>();
  std::vector<LogicalType> ignored;
  BuildKeyExec(BuildKeys(), *ls->build_exec_, ignored);
  ls->probe_exec_ = std::make_unique<ExpressionExecutor>();
  ignored.clear();
  BuildKeyExec(ProbeKeys(), *ls->probe_exec_, ignored);
  return ls;
}

auto PhysicalGraceHashJoin::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                    LocalSourceState &lstate) const -> SourceResultType {
  auto &src = static_cast<GraceGlobalSourceState &>(gstate);
  auto &ls = static_cast<GraceLocalSourceState &>(lstate);
  auto &gs = *src.sink_;
  const bool left = IsLeftJoin();
  const auto out_types = output.GetTypes();
  const idx_t build_cols = left ? out_types.size() - left_column_count_ : left_column_count_;
  const OutputLayout out{out_types, left, build_cols,
                         /*build_offset=*/left ? left_column_count_ : 0,
                         /*probe_offset=*/left ? 0 : build_cols};

  // Each step is task-local except ClaimWork; a step that made progress loops back to the top so
  // its staged rows go out one chunk per call (the re-entrancy protocol).
  while (true) {
    if (EmitStaged(ls, output)) {
      return SourceResultType::HAVE_MORE_OUTPUT;
    }
    if (DrainNullRows(gs, src, ls, out)) {
      continue;
    }
    if (AdvancePair(src, ls, out)) {
      continue;
    }
    PartitionPair pair;
    switch (ClaimWork(gs, src, ls, left, pair)) {
      case Claim::NULL_ROWS:
        continue;
      case Claim::PAIR:
        // Open outside the lock (a split publishes sub-pairs and loops back to claim again).
        OpenPair(src, ls, pair);
        continue;
      case Claim::NOTHING:
        // A task still splitting a pair will consume its own sub-pairs, so finishing early here
        // never loses work — it only retires this task.
        output.SetCardinality(0);
        return SourceResultType::FINISHED;
    }
  }
}

void PhysicalGraceHashJoin::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  // Three pipelines: partition the build, then partition the probe (same sink — the phase, flipped by
  // the first Finalize, tells the sides apart), then this operator as the SOURCE of the join output.
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[BuildChildIdx()]->BuildPipelines(build, builder);
  auto &probe = builder.CreateChildPipeline(current, *this);
  children_[ProbeChildIdx()]->BuildPipelines(probe, builder);
  PipelineBuilder::AddDependency(probe, build);  // phase order: the build pass runs first
}

}  // namespace bumblebee
