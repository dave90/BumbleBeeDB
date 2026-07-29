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
#include "type/string_heap.h"
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

/**
 * Local-table group count past which a sink task switches to per-partition sub-tables. From then
 * on Combine hands WHOLE tables to the merge partitions (an owning-pointer push under the
 * partition mutex — zero row copies, zero hashing) instead of re-materializing every group into
 * transport buffers: on a ~100M-group aggregate that transport copy alone doubles peak memory and
 * costs a full pass. Low-cardinality aggregates never reach the threshold and keep today's path.
 */
constexpr idx_t kSinkPartitionThreshold = 3ULL << 19;  // 1.5M

/** The partition of a group hash. The TOP bits: the table directory indexes with the low bits, so
 * partition and bucket choice stay independent. */
auto PartitionOf(hash_t hash) -> idx_t { return static_cast<idx_t>(hash >> 59) & (kMergePartitions - 1); }

/** @return True when aggregate `a` is MIN/MAX over a VARCHAR — the string extreme needs off-row state. */
auto IsStringMinMax(AggregationType type, const LogicalType &out_type) -> bool {
  return (type == AggregationType::MinAggregate || type == AggregationType::MaxAggregate) &&
         out_type.GetPhysicalType() == PhysicalType::STRING;
}

/** @brief Per-aggregate flag: is this a string MIN/MAX (1) or a numeric aggregate (0)? */
auto StringMinMaxFlags(const std::vector<AggregationType> &agg_types, const Schema &schema, idx_t num_groups)
    -> std::vector<char> {
  std::vector<char> flags(agg_types.size(), 0);
  for (idx_t a = 0; a < agg_types.size(); a++) {
    flags[a] = IsStringMinMax(agg_types[a], schema.GetColumn(num_groups + a).GetType()) ? 1 : 0;
  }
  return flags;
}

/**
 * @brief The physical type of each aggregate's in-row value slot.
 *
 * A numeric aggregate accumulates a DOUBLE in place. A string MIN/MAX cannot store a growable
 * payload inside the fixed-width row, so its slot holds a BIGINT: a 1-based index (0 = "no value
 * yet") into the table's off-row `str_vals_`, whose bytes live in the table's own `StringHeap`.
 */
auto RowValueTypes(const std::vector<char> &str_flags) -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(str_flags.size());
  for (const char is_str : str_flags) {
    types.emplace_back(is_str ? LogicalTypeId::BIGINT : LogicalTypeId::DOUBLE);
  }
  return types;
}

/**
 * @brief The physical type of each aggregate's value column when the state is TRANSPORTED between
 * tasks (Combine → merge buffers): VARCHAR for a string MIN/MAX (the extreme rides across as a real
 * string, re-homed into the buffer's heap), DOUBLE otherwise.
 */
auto TransportValueTypes(const std::vector<char> &str_flags, const Schema &schema, idx_t num_groups)
    -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(str_flags.size());
  for (idx_t a = 0; a < str_flags.size(); a++) {
    types.emplace_back(str_flags[a] ? schema.GetColumn(num_groups + a).GetType()
                                    : LogicalType(LogicalTypeId::DOUBLE));
  }
  return types;
}

/**
 * @brief The group-table layout: the group-by columns (the key), then per aggregate a BIGINT count
 * and a value slot (`val_types[a]`) — the accumulator state lives INSIDE the row, next to its key.
 * One row = one group = one contiguous blob; no per-group heap allocations to chase (except a
 * string MIN/MAX, whose extreme lives off-row via the index slot). COUNT uses only the count; SUM
 * accumulates in the value; MIN/MAX keep the extreme in the value, where "state initialized" is
 * simply count > 0 (every non-NULL update bumps the count).
 */
auto GroupTableTypes(const std::vector<LogicalType> &group_types, const std::vector<LogicalType> &val_types)
    -> std::vector<LogicalType> {
  auto types = group_types;
  for (const auto &val_type : val_types) {
    types.emplace_back(LogicalTypeId::BIGINT);
    types.emplace_back(val_type);
  }
  return types;
}

/**
 * @brief One aggregation table: the group rows with embedded state, plus the layout offsets of each
 * aggregate's count/value slot. Used both for a sink task's local table and for a source task's
 * per-partition final table.
 *
 * String MIN/MAX aggregates keep their running extreme OFF the row: the row slot holds a 1-based
 * index into `str_vals_` (0 = unset), and the extreme's bytes are owned by `str_heap_`. Because the
 * table owns both, every group row referencing them stays valid for the table's whole lifetime.
 */
struct GroupedAggState {
  GroupedAggState(const std::vector<LogicalType> &group_types, const std::vector<AggregationType> &agg_types,
                  std::vector<char> str_flags, const std::vector<LogicalType> &row_val_types,
                  idx_t initial_capacity = PRLHashTable::INITIAL_CAPACITY)
      : ht_(GroupTableTypes(group_types, row_val_types), group_types.size(), /*null_equal_keys=*/true,
            initial_capacity),
        num_groups_(group_types.size()),
        agg_types_(agg_types),
        str_flags_(std::move(str_flags)) {
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
  std::vector<char> str_flags_;
  std::vector<idx_t> cnt_offs_;
  std::vector<idx_t> val_offs_;
  /** Off-row running extremes for string MIN/MAX (index-referenced from the rows); bytes in `str_heap_`. */
  std::vector<string_t> str_vals_;
  StringHeap str_heap_;

  /** @brief Fold one string into the group at `addr`'s extreme for aggregate `a` (allocates on first). */
  void FoldString(idx_t a, data_ptr_t addr, const string_t &incoming) {
    const auto val_off = val_offs_[a];
    const auto idx = Load<int64_t>(addr + val_off);
    if (idx == 0) {
      str_vals_.push_back(str_heap_.AddString(incoming));
      Store<int64_t>(static_cast<int64_t>(str_vals_.size()), addr + val_off);  // 1-based; 0 stays "unset"
      return;
    }
    auto &cur = str_vals_[idx - 1];
    const bool take = agg_types_[a] == AggregationType::MinAggregate ? incoming < cur : incoming > cur;
    if (take) {
      cur = str_heap_.AddString(incoming);
    }
  }

  /**
   * @brief Sink path: find-or-create the groups of `group_chunk`; new groups start with zeroed state
   * (the zero constants ride the scatter). Returns the group-row address of every input row.
   */
  void AddGroups(DataChunk &group_chunk, Vector &hashes, Vector &addresses) {
    const idx_t count = group_chunk.GetSize();
    if (!add_scratch_ready_) {
      // Reusable: a partitioned sink calls this up to kMergePartitions times per input chunk,
      // so a per-call Initialize (an allocation per column) would dominate small sub-batches.
      add_scratch_.Initialize(ht_.GetTypes());
      add_scratch_ready_ = true;
    }
    for (idx_t g = 0; g < num_groups_; g++) {
      add_scratch_.data_[g].Reference(group_chunk.data_[g]);
    }
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      add_scratch_.data_[num_groups_ + 2 * a].Reference(zero_cnt_);
      add_scratch_.data_[num_groups_ + 2 * a + 1].Reference(str_flags_[a] ? zero_idx_ : zero_dbl_);
    }
    add_scratch_.SetCardinality(count);
    ht_.FindOrCreateGroups(hashes, add_scratch_, addresses);
  }

  /** AddGroups scratch (see there) plus the shared zero-state constants new groups scatter with. */
  DataChunk add_scratch_;
  bool add_scratch_ready_{false};
  Vector zero_cnt_{Value(static_cast<int64_t>(0))};
  Vector zero_dbl_{Value(static_cast<double>(0))};
  Vector zero_idx_{Value(static_cast<int64_t>(0))};  // string MIN/MAX: 0 = "no extreme yet"

  /** @brief Fold a whole vector of string arguments into aggregate `a` for each row's group. */
  void UpdateStringAggregate(idx_t a, Vector &arg, data_ptr_t *addrs, idx_t count) {
    VectorData vdata;
    arg.Orrify(count, vdata);
    const auto *data = reinterpret_cast<const string_t *>(vdata.data_);
    const auto cnt_off = cnt_offs_[a];
    for (idx_t i = 0; i < count; i++) {
      const idx_t row = vdata.sel_->GetIndex(i);
      if (vdata.validity_ != nullptr && !vdata.validity_->RowIsValid(row)) {
        continue;  // NULL argument: MIN/MAX skip it
      }
      auto *addr = addrs[i];
      FoldString(a, addr, data[row]);
      Store<int64_t>(Load<int64_t>(addr + cnt_off) + 1, addr + cnt_off);
    }
  }

  /**
   * @brief Merge path: fold a full-layout chunk (group columns + carried state columns) in.
   *
   * A row whose group is NEW carries its numeric state into the table via the scatter itself; a row
   * whose group already exists merges state-into-state (counts add; SUM adds values; MIN/MAX keep
   * the extreme, where a zero source count means "no state to merge"). String MIN/MAX never rides
   * the scatter (the row slot is only an index): its extreme is folded off-row for new and existing
   * groups alike, and its count is bumped exactly like the numeric aggregates.
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

    // Scatter new groups using a ROW-typed view: the transported string columns are VARCHAR, but the
    // row's string slot is a BIGINT index, so a new group's string slot must start at 0 (unset) —
    // its extreme is folded off-row below, never through the scatter.
    Vector zero_idx{Value(static_cast<int64_t>(0))};
    DataChunk row_view;
    row_view.Initialize(ht_.GetTypes());
    for (idx_t g = 0; g < num_groups_; g++) {
      row_view.data_[g].Reference(full_chunk.data_[g]);
    }
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      row_view.data_[num_groups_ + 2 * a].Reference(full_chunk.data_[num_groups_ + 2 * a]);  // count
      if (str_flags_[a]) {
        row_view.data_[num_groups_ + 2 * a + 1].Reference(zero_idx);
      } else {
        row_view.data_[num_groups_ + 2 * a + 1].Reference(full_chunk.data_[num_groups_ + 2 * a + 1]);
      }
    }
    row_view.SetCardinality(count);

    Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, count};
    SelectionVector new_sel(count);
    idx_t new_count = 0;
    ht_.FindOrCreateGroups(hashes, row_view, addresses, &new_sel, &new_count);

    std::array<bool, STANDARD_VECTOR_SIZE> is_new{};
    for (idx_t k = 0; k < new_count; k++) {
      is_new[new_sel.GetIndex(k)] = true;
    }

    auto *addrs = FlatVector::GetData<data_ptr_t>(addresses);
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      full_chunk.data_[num_groups_ + 2 * a].Normalify(count);
      const auto *src_cnt = FlatVector::GetData<int64_t>(full_chunk.data_[num_groups_ + 2 * a]);
      const auto cnt_off = cnt_offs_[a];
      const auto val_off = val_offs_[a];
      const auto type = agg_types_[a];

      if (str_flags_[a]) {
        // A new group's count rode the scatter; an existing group's count adds. The extreme is
        // folded off-row for both (the index slot started unset for a new group).
        full_chunk.data_[num_groups_ + 2 * a + 1].Normalify(count);
        const auto *src_val = FlatVector::GetData<string_t>(full_chunk.data_[num_groups_ + 2 * a + 1]);
        for (idx_t i = 0; i < count; i++) {
          if (src_cnt[i] == 0) {
            continue;
          }
          auto *addr = addrs[i];
          FoldString(a, addr, src_val[i]);
          if (!is_new[i]) {
            Store<int64_t>(Load<int64_t>(addr + cnt_off) + src_cnt[i], addr + cnt_off);
          }
        }
        continue;
      }

      full_chunk.data_[num_groups_ + 2 * a + 1].Normalify(count);
      const auto *src_val = FlatVector::GetData<double>(full_chunk.data_[num_groups_ + 2 * a + 1]);
      for (idx_t i = 0; i < count; i++) {
        if (is_new[i] || src_cnt[i] == 0) {
          continue;  // a new group's state rode the scatter; an empty source state merges to nothing
        }
        auto *addr = addrs[i];
        const auto dst_cnt = Load<int64_t>(addr + cnt_off);
        if (type == AggregationType::SumAggregate || type == AggregationType::AvgAggregate) {
          // AVG merges like SUM: add the partial value sums (and counts, handled above).
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
    /** Whole per-partition sub-tables handed off by partitioned sink tasks (zero-copy Combine);
     * the source adopts the largest and merges the rest into it. */
    std::vector<std::unique_ptr<GroupedAggState>> tables_;
  };

  std::vector<LogicalType> group_types_;
  std::vector<char> str_flags_;
  std::vector<LogicalType> row_val_types_;
  /** Row layout (string value slots are BIGINT indices) — the hash table and the local/final scans. */
  std::vector<LogicalType> row_types_;
  /** Transport layout (string value columns are VARCHAR) — the merge buffers crossing tasks. */
  std::vector<LogicalType> transport_types_;
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

  /** Adaptive sink partitioning (see kSinkPartitionThreshold): once `state_` outgrows the
   * threshold it is split by group hash into `parts_` and further inserts go straight to their
   * partition's sub-table; Combine then hands the sub-tables off whole. */
  bool partitioned_{false};
  std::array<std::unique_ptr<GroupedAggState>, kMergePartitions> parts_;
  std::vector<char> str_flags_;
  std::vector<LogicalType> row_val_types_;
  std::vector<LogicalType> row_types_;
  std::vector<LogicalType> transport_types_;
  /** Partitioned-insert scratch: per-partition selections and the sliced group/argument views. */
  std::array<std::unique_ptr<SelectionVector>, kMergePartitions> psels_;
  DataChunk group_sub_;
  DataChunk args_sub_;
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
  gs->str_flags_ = StringMinMaxFlags(agg_types_, *output_schema_, group_bys_.size());
  gs->row_val_types_ = RowValueTypes(gs->str_flags_);
  gs->row_types_ = GroupTableTypes(gs->group_types_, gs->row_val_types_);
  gs->transport_types_ =
      GroupTableTypes(gs->group_types_, TransportValueTypes(gs->str_flags_, *output_schema_, group_bys_.size()));
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
  auto str_flags = StringMinMaxFlags(agg_types_, *output_schema_, group_bys_.size());
  auto row_val_types = RowValueTypes(str_flags);
  ls->str_flags_ = str_flags;
  ls->row_val_types_ = row_val_types;
  ls->row_types_ = GroupTableTypes(ls->group_types_, row_val_types);
  ls->transport_types_ =
      GroupTableTypes(ls->group_types_, TransportValueTypes(str_flags, *output_schema_, group_bys_.size()));
  ls->state_ = std::make_unique<GroupedAggState>(ls->group_types_, agg_types_, std::move(str_flags), row_val_types);
  ls->group_chunk_.Initialize(ls->group_types_);
  ls->args_.Initialize(ls->arg_types_);
  ls->group_sub_.Initialize(ls->group_types_);
  ls->args_sub_.Initialize(ls->arg_types_);
  return ls;
}

namespace {
void RepartitionLocal(HashAggLocalSinkState &ls, const std::vector<AggregationType> &agg_types);
}  // namespace

auto PhysicalHashAggregate::Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState & /*gstate*/,
                                 LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<HashAggLocalSinkState &>(lstate);
  const idx_t count = input.GetSize();
  const idx_t partition_threshold = context.client_.config_.agg_partition_threshold_ > 0
                                        ? context.client_.config_.agg_partition_threshold_
                                        : kSinkPartitionThreshold;

  ls.group_exec_->Execute(input, ls.group_chunk_);
  ls.agg_exec_->Execute(input, ls.args_);

  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
  ls.group_chunk_.Hash(hashes);

  if (!ls.partitioned_) {
    Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, count};
    ls.state_->AddGroups(ls.group_chunk_, hashes, addresses);

    // Fold each aggregate's whole argument vector into the row-embedded states: one typed columnar
    // kernel per aggregate for the numeric ones, an off-row lexicographic fold for string MIN/MAX.
    auto *addrs = FlatVector::GetData<data_ptr_t>(addresses);
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      if (ls.state_->str_flags_[a]) {
        ls.state_->UpdateStringAggregate(a, ls.args_.data_[a], addrs, count);
      } else {
        UpdateOneAggregate(agg_types_[a], ls.args_.data_[a], addrs, count, ls.state_->cnt_offs_[a],
                           ls.state_->val_offs_[a]);
      }
    }
    if (ls.state_->ht_.Count() >= partition_threshold) {
      RepartitionLocal(ls, agg_types_);
    }
    return SinkResultType::NEED_MORE_INPUT;
  }

  // Partitioned path: scatter the chunk by group hash and insert each slice into its partition's
  // sub-table — the same per-row work as above, just against smaller (cache-friendlier) tables,
  // and Combine can later hand every sub-table off without touching a row.
  hashes.Normalify(count);
  const auto *hash_data = FlatVector::GetData<hash_t>(hashes);
  std::array<idx_t, kMergePartitions> counts{};
  for (idx_t i = 0; i < count; i++) {
    const idx_t p = PartitionOf(hash_data[i]);
    if (ls.psels_[p] == nullptr) {
      ls.psels_[p] = std::make_unique<SelectionVector>(STANDARD_VECTOR_SIZE);
    }
    ls.psels_[p]->SetIndex(counts[p]++, i);
  }
  for (idx_t p = 0; p < kMergePartitions; p++) {
    const idx_t n = counts[p];
    if (n == 0) {
      continue;
    }
    if (ls.parts_[p] == nullptr) {
      ls.parts_[p] = std::make_unique<GroupedAggState>(ls.group_types_, agg_types_, ls.str_flags_, ls.row_val_types_,
                                                       PRLHashTable::INITIAL_CAPACITY * 16);
    }
    auto &part = *ls.parts_[p];
    ls.group_sub_.Slice(ls.group_chunk_, *ls.psels_[p], n);
    ls.args_sub_.Slice(ls.args_, *ls.psels_[p], n);
    Vector hashes_sub{LogicalType{LogicalTypeId::UBIGINT}, n};
    auto *hs = FlatVector::GetData<hash_t>(hashes_sub);
    for (idx_t k = 0; k < n; k++) {
      hs[k] = hash_data[ls.psels_[p]->GetIndex(k)];
    }
    Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, n};
    part.AddGroups(ls.group_sub_, hashes_sub, addresses);
    auto *addrs = FlatVector::GetData<data_ptr_t>(addresses);
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      if (part.str_flags_[a]) {
        part.UpdateStringAggregate(a, ls.args_sub_.data_[a], addrs, n);
      } else {
        UpdateOneAggregate(agg_types_[a], ls.args_sub_.data_[a], addrs, n, part.cnt_offs_[a], part.val_offs_[a]);
      }
    }
  }
  return SinkResultType::NEED_MORE_INPUT;
}

namespace {

/** @brief Append `sel[0..n)` of `src` (transport layout) to a partition's row buffers, packing chunks full. */
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

/**
 * @brief Materialize a scanned row chunk (string value slots = indices) into a transport chunk
 * (string value columns = real VARCHAR), so the state can cross the merge boundary as strings.
 *
 * Numeric columns (groups, counts, numeric values) are referenced zero-copy; a string MIN/MAX value
 * column is rebuilt from `state.str_vals_` (an unset index — the group saw no non-NULL string —
 * becomes an empty string, which the merge drops because its count is 0).
 */
void BuildTransportChunk(const GroupedAggState &state, idx_t num_groups, DataChunk &row_chunk, DataChunk &out);

/**
 * @brief Split a sink task's single local table into per-partition sub-tables (one-time, on
 * crossing kSinkPartitionThreshold): scan it in transport form and merge each row into its hash
 * partition's sub-table. The stored row hashes drive the partitioning — no key is re-hashed.
 */
void RepartitionLocal(HashAggLocalSinkState &ls, const std::vector<AggregationType> &agg_types) {
  auto old = std::move(ls.state_);
  const idx_t num_groups = ls.group_types_.size();
  const auto &stored_hashes = old->ht_.RowHashes();

  DataChunk scan_chunk;
  scan_chunk.Initialize(ls.row_types_);
  DataChunk transport_chunk;
  transport_chunk.Initialize(ls.transport_types_);
  DataChunk pack_chunk;
  pack_chunk.Initialize(ls.transport_types_);
  std::array<idx_t, kMergePartitions> counts{};

  const idx_t sub_capacity =
      std::max<idx_t>(PRLHashTable::INITIAL_CAPACITY,
                      static_cast<idx_t>(static_cast<double>(old->ht_.Count()) / kMergePartitions * 3));

  idx_t offset = 0;
  while (true) {
    const idx_t n = old->ht_.Scan(offset, scan_chunk);
    if (n == 0) {
      break;
    }
    BuildTransportChunk(*old, num_groups, scan_chunk, transport_chunk);
    counts.fill(0);
    for (idx_t i = 0; i < n; i++) {
      const idx_t p = PartitionOf(stored_hashes[offset + i]);
      if (ls.psels_[p] == nullptr) {
        ls.psels_[p] = std::make_unique<SelectionVector>(STANDARD_VECTOR_SIZE);
      }
      ls.psels_[p]->SetIndex(counts[p]++, i);
    }
    for (idx_t p = 0; p < kMergePartitions; p++) {
      if (counts[p] == 0) {
        continue;
      }
      if (ls.parts_[p] == nullptr) {
        ls.parts_[p] =
            std::make_unique<GroupedAggState>(ls.group_types_, agg_types, ls.str_flags_, ls.row_val_types_,
                                              sub_capacity);
      }
      pack_chunk.Reset();
      for (idx_t c = 0; c < ls.transport_types_.size(); c++) {
        VectorOperations::Copy(transport_chunk.data_[c], pack_chunk.data_[c], *ls.psels_[p], counts[p], 0, 0);
      }
      pack_chunk.SetCardinality(counts[p]);
      ls.parts_[p]->MergeChunk(pack_chunk);
    }
    offset += n;
    scan_chunk.Reset();
  }
  ls.partitioned_ = true;
}

void BuildTransportChunk(const GroupedAggState &state, idx_t num_groups, DataChunk &row_chunk, DataChunk &out) {
  const idx_t n = row_chunk.GetSize();
  out.Reset();
  for (idx_t c = 0; c < num_groups; c++) {
    out.data_[c].Reference(row_chunk.data_[c]);
  }
  for (idx_t a = 0; a < state.agg_types_.size(); a++) {
    const idx_t cnt_col = num_groups + 2 * a;
    const idx_t val_col = num_groups + 2 * a + 1;
    out.data_[cnt_col].Reference(row_chunk.data_[cnt_col]);
    if (!state.str_flags_[a]) {
      out.data_[val_col].Reference(row_chunk.data_[val_col]);
      continue;
    }
    Vector &dst = out.data_[val_col];
    dst.SetVectorType(VectorType::FLAT_VECTOR);
    const auto *idxs = FlatVector::GetData<int64_t>(row_chunk.data_[val_col]);
    auto *strs = FlatVector::GetData<string_t>(dst);
    for (idx_t i = 0; i < n; i++) {
      const auto idx = idxs[i];
      strs[i] = idx == 0 ? StringVector::AddString(dst, "", 0)
                         : StringVector::AddString(dst, state.str_vals_[idx - 1]);
    }
  }
  out.SetCardinality(n);
}

}  // namespace

void PhysicalHashAggregate::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                    LocalSinkState &lstate) const {
  auto &gs = static_cast<HashAggGlobalSinkState &>(gstate);
  auto &ls = static_cast<HashAggLocalSinkState &>(lstate);

  // A partitioned task hands each sub-table off WHOLE: an owning-pointer push under the partition
  // mutex — no row is copied, no key re-hashed. (The strings ride along: the sub-table owns its
  // heap.) The source merges the handed-off tables partition-wise, in parallel.
  if (ls.partitioned_) {
    for (idx_t p = 0; p < kMergePartitions; p++) {
      if (ls.parts_[p] == nullptr || ls.parts_[p]->ht_.Count() == 0) {
        continue;
      }
      auto &part = gs.partitions_[p];
      std::lock_guard lock(part.mu_);
      part.tables_.push_back(std::move(ls.parts_[p]));
    }
    return;
  }

  // Scatter this task's groups (with their carried state) into the hash partitions. Only the buffer
  // push is under a (per-partition) lock; no group is re-hashed into a global table here.
  DataChunk scan_chunk;
  scan_chunk.Initialize(gs.row_types_);
  DataChunk transport_chunk;
  transport_chunk.Initialize(gs.transport_types_);
  std::array<std::unique_ptr<SelectionVector>, kMergePartitions> sels;
  std::array<idx_t, kMergePartitions> counts{};

  // The table stored every group's key hash at insert time, in the same order Scan walks the
  // rows — re-hashing the group columns here would repeat that work for every group.
  const auto &stored_hashes = ls.state_->ht_.RowHashes();

  idx_t offset = 0;
  while (true) {
    const idx_t n = ls.state_->ht_.Scan(offset, scan_chunk);
    if (n == 0) {
      break;
    }
    // Turn the scanned indices into real strings before the state leaves this task.
    BuildTransportChunk(*ls.state_, ls.group_types_.size(), scan_chunk, transport_chunk);

    const hash_t *hash_data = stored_hashes.data() + offset;

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
      PartitionAppend(part, gs.transport_types_, transport_chunk, *sels[p], counts[p]);
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
                         const std::vector<AggregationType> &agg_types, const std::vector<char> &str_flags,
                         const std::vector<LogicalType> &row_val_types) -> std::unique_ptr<GroupedAggState> {
  // The buffered rows + handed-off sub-tables bound the partition's group count exactly, so the
  // directory can be sized once — a high-cardinality partition (millions of mostly-unique groups)
  // would otherwise pay a dozen grow-and-rehash passes on the way up from the default capacity.
  idx_t total_rows = 0;
  for (const auto &chunk : part.rows_) {
    total_rows += chunk->GetSize();
  }
  for (const auto &t : part.tables_) {
    total_rows += t->ht_.Count();
  }
  idx_t capacity = PRLHashTable::INITIAL_CAPACITY;
  while (static_cast<double>(capacity) * PRLHashTable::LOAD_FACTOR <= static_cast<double>(total_rows)) {
    capacity <<= 1;
  }

  // Adopt the largest handed-off sub-table wholesale (its rows never move again); everything else
  // merges into it.
  std::unique_ptr<GroupedAggState> table;
  if (!part.tables_.empty()) {
    idx_t largest = 0;
    for (idx_t i = 1; i < part.tables_.size(); i++) {
      if (part.tables_[i]->ht_.Count() > part.tables_[largest]->ht_.Count()) {
        largest = i;
      }
    }
    table = std::move(part.tables_[largest]);
    part.tables_.erase(part.tables_.begin() + static_cast<int64_t>(largest));
    if (table->ht_.Capacity() < capacity) {
      table->ht_.Resize(capacity);
    }
  } else {
    table = std::make_unique<GroupedAggState>(group_types, agg_types, str_flags, row_val_types, capacity);
  }

  const idx_t num_groups = group_types.size();
  if (!part.tables_.empty()) {
    DataChunk scan_chunk;
    scan_chunk.Initialize(table->ht_.GetTypes());
    // Transport layout: same as the row layout except string extremes travel as real VARCHARs
    // (row_val_types holds ONE value slot per aggregate; GroupTableTypes adds the counts).
    std::vector<LogicalType> transport_vals;
    transport_vals.reserve(agg_types.size());
    for (idx_t a = 0; a < agg_types.size(); a++) {
      transport_vals.push_back(str_flags[a] ? LogicalType(LogicalTypeId::STRING) : row_val_types[a]);
    }
    DataChunk transport_chunk;
    transport_chunk.Initialize(GroupTableTypes(group_types, transport_vals));
    for (auto &other : part.tables_) {
      idx_t offset = 0;
      while (true) {
        const idx_t n = other->ht_.Scan(offset, scan_chunk);
        if (n == 0) {
          break;
        }
        BuildTransportChunk(*other, num_groups, scan_chunk, transport_chunk);
        table->MergeChunk(transport_chunk);
        offset += n;
        scan_chunk.Reset();
      }
      other.reset();  // release eagerly: don't hold every sub-table and the final table to peak
    }
  }
  for (auto &chunk : part.rows_) {
    table->MergeChunk(*chunk);
  }
  part.rows_.clear();
  part.rows_.shrink_to_fit();
  part.tables_.clear();
  part.tables_.shrink_to_fit();
  return table;
}

/**
 * @brief Finalize ONE numeric aggregate's output column from its (count, value) state columns.
 *
 * COUNT emits the count; SUM/MIN/MAX emit the value with "zero non-NULL inputs" turned into NULL.
 * The state column is referenced zero-copy when its physical type already matches the output
 * column, cast in one vectorized pass otherwise. (String MIN/MAX is finalized separately, from the
 * off-row extremes, since its state column is an index rather than a value.)
 */
void FinalizeAggregateColumn(AggregationType type, Vector &cnt_vec, Vector &val_vec, Vector &out,
                             const LogicalType &out_type, idx_t n) {
  if (type == AggregationType::AvgAggregate) {
    // AVG = value-sum / count, as DOUBLE; a zero-count group (all inputs NULL) is NULL.
    const auto *cnt = FlatVector::GetData<int64_t>(cnt_vec);
    const auto *val = FlatVector::GetData<double>(val_vec);
    auto *out_data = FlatVector::GetData<double>(out);
    auto &validity = FlatVector::Validity(out);
    for (idx_t i = 0; i < n; i++) {
      if (cnt[i] == 0) {
        validity.SetInvalid(i);
      } else {
        out_data[i] = val[i] / static_cast<double>(cnt[i]);
      }
    }
    return;
  }
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

/** @brief Finalize a string MIN/MAX column: read each group's off-row extreme (NULL when count 0). */
void FinalizeStringAggregateColumn(const GroupedAggState &state, Vector &cnt_vec, Vector &val_vec, Vector &out,
                                   idx_t n) {
  const auto *cnt = FlatVector::GetData<int64_t>(cnt_vec);
  const auto *idxs = FlatVector::GetData<int64_t>(val_vec);
  out.SetVectorType(VectorType::FLAT_VECTOR);
  auto *out_data = FlatVector::GetData<string_t>(out);
  auto &validity = FlatVector::Validity(out);
  for (idx_t i = 0; i < n; i++) {
    if (cnt[i] == 0 || idxs[i] == 0) {
      validity.SetInvalid(i);  // no non-NULL string in the group
    } else {
      out_data[i] = StringVector::AddString(out, state.str_vals_[idxs[i] - 1]);
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
      if (part.rows_.empty() && part.tables_.empty()) {
        continue;
      }
      ls.table_ = BuildPartitionTable(part, sink.group_types_, agg_types_, sink.str_flags_, sink.row_val_types_);
      ls.scan_offset_ = 0;
    }

    // Emit the next chunk of the partition in hand; a drained partition goes back for the next one.
    DataChunk scan_chunk;
    scan_chunk.Initialize(sink.row_types_);
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
      Vector &cnt_vec = scan_chunk.data_[num_groups + 2 * a];
      Vector &val_vec = scan_chunk.data_[num_groups + 2 * a + 1];
      if (sink.str_flags_[a]) {
        FinalizeStringAggregateColumn(*ls.table_, cnt_vec, val_vec, output.data_[num_groups + a], n);
      } else {
        FinalizeAggregateColumn(agg_types_[a], cnt_vec, val_vec, output.data_[num_groups + a],
                                output_schema_->GetColumn(num_groups + a).GetType(), n);
      }
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
