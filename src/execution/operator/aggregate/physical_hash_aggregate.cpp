//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_hash_aggregate.cpp
//
// Identification: src/execution/operator/aggregate/physical_hash_aggregate.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/aggregate/physical_hash_aggregate.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "common/helper.h"
#include "execution/aggregate/aggregate_update_kernels.h"
#include "execution/expression_executor.h"
#include "execution/prl_hash_table.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/value.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

namespace {

/**
 * How many hash partitions the merge boundary uses. Combine scatters each task's local groups into
 * these buffers (one small mutex each — contention is 1/kMergePartitions of a single global lock),
 * and each source task then builds and emits one whole partition independently: the merge of a
 * high-cardinality GROUP BY parallelizes instead of serializing behind one mutex. A power of two.
 */
constexpr idx_t kMergePartitions = 32;

/** The partition of a group hash. The TOP bits: the table directory indexes with the low bits, so
 * partition and bucket choice stay independent. */
auto PartitionOf(hash_t hash) -> idx_t { return static_cast<idx_t>(hash >> 59) & (kMergePartitions - 1); }

/**
 * The group-table layout: the group-by columns (the key), then per aggregate a BIGINT count and a
 * DOUBLE value slot — the accumulator state lives INSIDE the row, next to its key. One row = one
 * group = one contiguous cache line-ish blob; no per-group heap allocations to chase. COUNT uses
 * only the count; SUM accumulates in the value; MIN/MAX keep the extreme in the value, where
 * "state initialized" is simply count > 0 (every non-NULL update bumps the count).
 */
auto GroupTableTypes(const std::vector<LogicalType> &group_types, idx_t num_aggs) -> std::vector<LogicalType> {
  auto types = group_types;
  for (idx_t a = 0; a < num_aggs; a++) {
    types.emplace_back(LogicalTypeId::BIGINT);
    types.emplace_back(LogicalTypeId::DOUBLE);
  }
  return types;
}

/**
 * @brief One aggregation table: the group rows with embedded state, plus the layout offsets of each
 * aggregate's count/value slot. Used both for a sink task's local table and for a source task's
 * per-partition final table.
 */
struct GroupedAggState {
  GroupedAggState(const std::vector<LogicalType> &group_types, const std::vector<AggregationType> &agg_types)
      : ht_(GroupTableTypes(group_types, agg_types.size()), group_types.size(), /*null_equal_keys=*/true),
        num_groups_(group_types.size()),
        agg_types_(agg_types) {
    const auto &offsets = ht_.GetLayout().GetOffsets();
    cnt_offs_.reserve(agg_types.size());
    val_offs_.reserve(agg_types.size());
    for (idx_t a = 0; a < agg_types.size(); a++) {
      cnt_offs_.push_back(offsets[num_groups_ + 2 * a]);
      val_offs_.push_back(offsets[num_groups_ + 2 * a + 1]);
    }
  }

  PRLHashTable ht_;
  idx_t num_groups_;
  std::vector<AggregationType> agg_types_;
  std::vector<idx_t> cnt_offs_;
  std::vector<idx_t> val_offs_;

  /**
   * @brief Sink path: find-or-create the groups of `group_chunk`; new groups start with zeroed state
   * (the zero constants ride the scatter). Returns the group-row address of every input row.
   */
  void AddGroups(DataChunk &group_chunk, Vector &hashes, Vector &addresses) {
    const idx_t count = group_chunk.GetSize();
    DataChunk table_chunk;
    table_chunk.Initialize(ht_.GetTypes());
    for (idx_t g = 0; g < num_groups_; g++) {
      table_chunk.data_[g].Reference(group_chunk.data_[g]);
    }
    Vector zero_cnt{Value(static_cast<int64_t>(0))};
    Vector zero_val{Value(static_cast<double>(0))};
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      table_chunk.data_[num_groups_ + 2 * a].Reference(zero_cnt);
      table_chunk.data_[num_groups_ + 2 * a + 1].Reference(zero_val);
    }
    table_chunk.SetCardinality(count);
    ht_.FindOrCreateGroups(hashes, table_chunk, addresses);
  }

  /**
   * @brief Merge path: fold a full-layout chunk (group columns + carried state columns) in.
   *
   * A row whose group is NEW carries its state into the table via the scatter itself; a row whose
   * group already exists merges state-into-state (counts add; SUM adds values; MIN/MAX keep the
   * extreme, where a zero source count means "no state to merge").
   */
  void MergeChunk(DataChunk &full_chunk) {
    const idx_t count = full_chunk.GetSize();
    if (count == 0) {
      return;
    }
    // Hash the group-key prefix only.
    DataChunk group_view;
    group_view.Initialize(std::vector<LogicalType>(ht_.GetTypes().begin(), ht_.GetTypes().begin() + num_groups_));
    for (idx_t g = 0; g < num_groups_; g++) {
      group_view.data_[g].Reference(full_chunk.data_[g]);
    }
    group_view.SetCardinality(count);
    Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
    group_view.Hash(hashes);

    Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, count};
    SelectionVector new_sel(count);
    idx_t new_count = 0;
    ht_.FindOrCreateGroups(hashes, full_chunk, addresses, &new_sel, &new_count);

    std::array<bool, STANDARD_VECTOR_SIZE> is_new{};
    for (idx_t k = 0; k < new_count; k++) {
      is_new[new_sel.GetIndex(k)] = true;
    }

    auto *addrs = FlatVector::GetData<data_ptr_t>(addresses);
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      full_chunk.data_[num_groups_ + 2 * a].Normalify(count);
      full_chunk.data_[num_groups_ + 2 * a + 1].Normalify(count);
      const auto *src_cnt = FlatVector::GetData<int64_t>(full_chunk.data_[num_groups_ + 2 * a]);
      const auto *src_val = FlatVector::GetData<double>(full_chunk.data_[num_groups_ + 2 * a + 1]);
      const auto cnt_off = cnt_offs_[a];
      const auto val_off = val_offs_[a];
      const auto type = agg_types_[a];
      for (idx_t i = 0; i < count; i++) {
        if (is_new[i] || src_cnt[i] == 0) {
          continue;  // a new group's state rode the scatter; an empty source state merges to nothing
        }
        auto *addr = addrs[i];
        const auto dst_cnt = Load<int64_t>(addr + cnt_off);
        if (type == AggregationType::SumAggregate) {
          Store<double>(Load<double>(addr + val_off) + src_val[i], addr + val_off);
        } else if (type == AggregationType::MinAggregate || type == AggregationType::MaxAggregate) {
          if (dst_cnt == 0) {
            Store<double>(src_val[i], addr + val_off);
          } else {
            const auto cur = Load<double>(addr + val_off);
            const bool take_src =
                type == AggregationType::MinAggregate ? src_val[i] < cur : src_val[i] > cur;
            if (take_src) {
              Store<double>(src_val[i], addr + val_off);
            }
          }
        }
        Store<int64_t>(dst_cnt + src_cnt[i], addr + cnt_off);
      }
    }
  }
};

}  // namespace

struct HashAggGlobalSinkState : GlobalSinkState {
  /**
   * One merge partition: the full-layout rows (groups + carried state) every task's Combine
   * scattered here. Its mutex only guards the buffer push — the expensive work (building the
   * final table) happens later, in parallel, one partition per source task.
   */
  struct MergePartition {
    std::mutex mu_;
    std::vector<std::unique_ptr<DataChunk>> rows_;
  };

  std::vector<LogicalType> group_types_;
  std::vector<LogicalType> table_types_;
  std::array<MergePartition, kMergePartitions> partitions_;
};

struct HashAggLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> group_exec_;
  std::unique_ptr<ExpressionExecutor> agg_exec_;
  std::vector<LogicalType> group_types_;
  std::vector<LogicalType> arg_types_;
  std::unique_ptr<GroupedAggState> state_;
  /** Reused per Sink call: Execute() re-references their columns, so no per-chunk allocation. */
  DataChunk group_chunk_;
  DataChunk args_;
};

struct HashAggGlobalSourceState : GlobalSourceState {
  GlobalSinkState *sink_{nullptr};
  /** The next merge partition to hand out; each source task builds and drains whole partitions. */
  std::atomic<idx_t> next_partition_{0};
  auto MaxThreads() -> idx_t override { return kMergePartitions; }
};

struct HashAggLocalSourceState : LocalSourceState {
  /** The partition table this task is currently emitting, and the scan cursor into it. */
  std::unique_ptr<GroupedAggState> table_;
  idx_t scan_offset_{0};
};

auto PhysicalHashAggregate::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  auto gs = std::make_unique<HashAggGlobalSinkState>();
  gs->group_types_.reserve(group_bys_.size());
  for (const auto &g : group_bys_) {
    gs->group_types_.push_back(g->GetReturnType().GetType());
  }
  gs->table_types_ = GroupTableTypes(gs->group_types_, agg_types_.size());
  return gs;
}

auto PhysicalHashAggregate::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  auto ls = std::make_unique<HashAggLocalSinkState>();
  ls->group_exec_ = std::make_unique<ExpressionExecutor>();
  for (const auto &g : group_bys_) {
    ls->group_exec_->AddExpression(*g);
    ls->group_types_.push_back(g->GetReturnType().GetType());
  }
  ls->agg_exec_ = std::make_unique<ExpressionExecutor>();
  for (const auto &a : aggregates_) {
    ls->agg_exec_->AddExpression(*a);
    ls->arg_types_.push_back(a->GetReturnType().GetType());
  }
  ls->state_ = std::make_unique<GroupedAggState>(ls->group_types_, agg_types_);
  ls->group_chunk_.Initialize(ls->group_types_);
  ls->args_.Initialize(ls->arg_types_);
  return ls;
}

auto PhysicalHashAggregate::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                 LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<HashAggLocalSinkState &>(lstate);
  const idx_t count = input.GetSize();

  ls.group_exec_->Execute(input, ls.group_chunk_);
  ls.agg_exec_->Execute(input, ls.args_);

  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
  ls.group_chunk_.Hash(hashes);
  Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, count};
  ls.state_->AddGroups(ls.group_chunk_, hashes, addresses);

  // Fold each aggregate's whole argument vector into the row-embedded states: one typed columnar
  // kernel per aggregate, no per-row Value boxing.
  auto *addrs = FlatVector::GetData<data_ptr_t>(addresses);
  for (idx_t a = 0; a < agg_types_.size(); a++) {
    UpdateOneAggregate(agg_types_[a], ls.args_.data_[a], addrs, count, ls.state_->cnt_offs_[a],
                       ls.state_->val_offs_[a]);
  }
  return SinkResultType::NEED_MORE_INPUT;
}

namespace {

/** @brief Append `sel[0..n)` of `src` (full layout) to a partition's row buffers, packing chunks full. */
void PartitionAppend(HashAggGlobalSinkState::MergePartition &part, const std::vector<LogicalType> &types,
                     DataChunk &src, const SelectionVector &sel, idx_t n) {
  idx_t done = 0;
  while (done < n) {
    if (part.rows_.empty() || part.rows_.back()->GetSize() >= STANDARD_VECTOR_SIZE) {
      auto chunk = std::make_unique<DataChunk>();
      chunk->Initialize(types);
      chunk->SetCardinality(0);
      part.rows_.push_back(std::move(chunk));
    }
    auto &dst = *part.rows_.back();
    const idx_t fill = dst.GetSize();
    const idx_t take = std::min<idx_t>(n - done, STANDARD_VECTOR_SIZE - fill);
    for (idx_t c = 0; c < types.size(); c++) {
      // Selection copy with a target offset; string payloads are re-homed into the buffer's heaps.
      VectorOperations::Copy(src.data_[c], dst.data_[c], sel, done + take, done, fill);
    }
    dst.SetCardinality(fill + take);
    done += take;
  }
}

}  // namespace

void PhysicalHashAggregate::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                    LocalSinkState &lstate) const {
  auto &gs = static_cast<HashAggGlobalSinkState &>(gstate);
  auto &ls = static_cast<HashAggLocalSinkState &>(lstate);

  // Scatter this task's groups (with their carried state) into the hash partitions. Only the buffer
  // push is under a (per-partition) lock; no group is re-hashed into a global table here.
  DataChunk scan_chunk;
  scan_chunk.Initialize(gs.table_types_);
  DataChunk group_view;
  group_view.Initialize(ls.group_types_);
  std::array<std::unique_ptr<SelectionVector>, kMergePartitions> sels;
  std::array<idx_t, kMergePartitions> counts{};

  idx_t offset = 0;
  while (true) {
    const idx_t n = ls.state_->ht_.Scan(offset, scan_chunk);
    if (n == 0) {
      break;
    }
    for (idx_t g = 0; g < ls.group_types_.size(); g++) {
      group_view.data_[g].Reference(scan_chunk.data_[g]);
    }
    group_view.SetCardinality(n);
    Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, n};
    group_view.Hash(hashes);
    const auto *hash_data = FlatVector::GetData<hash_t>(hashes);

    counts.fill(0);
    for (idx_t i = 0; i < n; i++) {
      const idx_t p = PartitionOf(hash_data[i]);
      if (sels[p] == nullptr) {
        sels[p] = std::make_unique<SelectionVector>(STANDARD_VECTOR_SIZE);
      }
      sels[p]->SetIndex(counts[p]++, i);
    }
    for (idx_t p = 0; p < kMergePartitions; p++) {
      if (counts[p] == 0) {
        continue;
      }
      auto &part = gs.partitions_[p];
      std::lock_guard lock(part.mu_);
      PartitionAppend(part, gs.table_types_, scan_chunk, *sels[p], counts[p]);
    }
    offset += n;
    scan_chunk.Reset();
  }
}

auto PhysicalHashAggregate::Finalize(ClientContext & /*context*/, GlobalSinkState & /*gstate*/, idx_t /*stage*/,
                                     idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  // Nothing to do: the partitions were buffered by Combine, and the SOURCE builds them in parallel
  // (Finalize runs single-task, so building here would serialize the merge again).
  return SinkFinalizeType::READY;
}

auto PhysicalHashAggregate::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<HashAggGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalHashAggregate::GetLocalSourceState(ExecutionContext & /*context*/, GlobalSourceState & /*gstate*/) const
    -> std::unique_ptr<LocalSourceState> {
  return std::make_unique<HashAggLocalSourceState>();
}

namespace {

/**
 * @brief Build one partition's final table from its buffered rows, then FREE the buffers.
 *
 * Every buffered row either creates its group (its state rides the scatter) or merges into it.
 * Partitions are disjoint by hash, so each is built fully independently — this call, one per
 * source task, is exactly the merge work that runs in parallel. The row buffers are released
 * eagerly: once merged they are dead weight, and a large aggregation should not hold both the
 * buffers and the tables to peak.
 */
auto BuildPartitionTable(HashAggGlobalSinkState::MergePartition &part, const std::vector<LogicalType> &group_types,
                         const std::vector<AggregationType> &agg_types) -> std::unique_ptr<GroupedAggState> {
  auto table = std::make_unique<GroupedAggState>(group_types, agg_types);
  for (auto &chunk : part.rows_) {
    table->MergeChunk(*chunk);
  }
  part.rows_.clear();
  part.rows_.shrink_to_fit();
  return table;
}

/**
 * @brief Finalize ONE aggregate's output column from its (count, value) state columns.
 *
 * COUNT emits the count; SUM/MIN/MAX emit the value with "zero non-NULL inputs" turned into NULL.
 * The state column is referenced zero-copy when its physical type already matches the output
 * column, cast in one vectorized pass otherwise.
 */
void FinalizeAggregateColumn(AggregationType type, Vector &cnt_vec, Vector &val_vec, Vector &out,
                             const LogicalType &out_type, idx_t n) {
  const bool is_count =
      type == AggregationType::CountStarAggregate || type == AggregationType::CountAggregate;
  Vector &result_src = is_count ? cnt_vec : val_vec;
  if (result_src.GetLogicalType().GetPhysicalType() == out_type.GetPhysicalType()) {
    out.Reference(result_src);
  } else {
    VectorOperations::Cast(result_src, out, n);
  }
  if (is_count) {
    return;  // a count is never NULL
  }
  const auto *cnt = FlatVector::GetData<int64_t>(cnt_vec);
  auto &validity = FlatVector::Validity(out);
  for (idx_t i = 0; i < n; i++) {
    if (cnt[i] == 0) {
      validity.SetInvalid(i);
    }
  }
}

}  // namespace

auto PhysicalHashAggregate::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                    LocalSourceState &lstate) const -> SourceResultType {
  auto &src = static_cast<HashAggGlobalSourceState &>(gstate);
  auto &ls = static_cast<HashAggLocalSourceState &>(lstate);
  auto &sink = *static_cast<HashAggGlobalSinkState *>(src.sink_);
  const idx_t num_groups = group_bys_.size();

  while (true) {
    // No partition in hand: claim the next non-empty one and build its table.
    if (ls.table_ == nullptr) {
      const idx_t p = src.next_partition_.fetch_add(1, std::memory_order_relaxed);
      if (p >= kMergePartitions) {
        return SourceResultType::FINISHED;
      }
      auto &part = sink.partitions_[p];
      if (part.rows_.empty()) {
        continue;
      }
      ls.table_ = BuildPartitionTable(part, sink.group_types_, agg_types_);
      ls.scan_offset_ = 0;
    }

    // Emit the next chunk of the partition in hand; a drained partition goes back for the next one.
    DataChunk scan_chunk;
    scan_chunk.Initialize(sink.table_types_);
    const idx_t n = ls.table_->ht_.Scan(ls.scan_offset_, scan_chunk, /*copy_strings=*/true);
    if (n == 0) {
      ls.table_.reset();
      continue;
    }
    ls.scan_offset_ += n;

    for (idx_t g = 0; g < num_groups; g++) {
      output.data_[g].Reference(scan_chunk.data_[g]);
    }
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      FinalizeAggregateColumn(agg_types_[a], scan_chunk.data_[num_groups + 2 * a],
                              scan_chunk.data_[num_groups + 2 * a + 1], output.data_[num_groups + a],
                              output_schema_->GetColumn(num_groups + a).GetType(), n);
    }
    output.SetCardinality(n);
    return SourceResultType::HAVE_MORE_OUTPUT;
  }
}

void PhysicalHashAggregate::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
