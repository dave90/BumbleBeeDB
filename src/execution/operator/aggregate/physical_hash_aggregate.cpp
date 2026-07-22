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

#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "common/helper.h"
#include "execution/aggregate/aggregate_state.h"
#include "execution/expression_executor.h"
#include "execution/prl_hash_table.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/value.h"

namespace bumblebee {

namespace {

/**
 * The group table's layout is the group-by columns followed by one hidden BIGINT ordinal column. The
 * ordinal is each group's creation index: it addresses the accumulator set of the group from a row
 * address, and because groups are created in insertion order it equals the group's Scan position.
 */
auto GroupTableTypes(const std::vector<LogicalType> &group_types) -> std::vector<LogicalType> {
  auto types = group_types;
  types.emplace_back(LogicalTypeId::BIGINT);
  return types;
}

/** @brief One accumulator per aggregate, for one group. */
auto MakeAccums(const std::vector<AggregationType> &agg_types) -> std::vector<AggregateAccumulator> {
  std::vector<AggregateAccumulator> accums;
  accums.reserve(agg_types.size());
  for (auto type : agg_types) {
    accums.emplace_back(type);
  }
  return accums;
}

/**
 * @brief The shared table state of one aggregation side (a task-local one, or the global one).
 *
 * `ht` dedups on the group columns (NULL groups compare equal — SQL GROUP BY semantics); `accums[g]`
 * is group g's accumulator set, indexed by the hidden ordinal column.
 */
struct GroupedAggState {
  explicit GroupedAggState(const std::vector<LogicalType> &group_types)
      : ht_(GroupTableTypes(group_types), group_types.size(), /*null_equal_keys=*/true),
        ordinal_offset_(ht_.GetLayout().GetOffsets().back()) {}

  PRLHashTable ht_;
  idx_t ordinal_offset_;
  std::vector<std::vector<AggregateAccumulator>> accums_;

  /**
   * @brief Find-or-create the groups of `group_chunk` and return the addresses vector.
   *
   * New groups get their ordinal stamped into the row and a fresh accumulator set appended.
   */
  void AddGroups(DataChunk &group_chunk, const std::vector<AggregationType> &agg_types, Vector &addresses) {
    const idx_t count = group_chunk.GetSize();

    // The table chunk = the group columns plus the (uninitialized) ordinal column; the ordinal is
    // stamped after the scatter, once a row is known to be a new group.
    DataChunk table_chunk;
    table_chunk.Initialize(ht_.GetTypes());
    for (idx_t g = 0; g + 1 < ht_.GetTypes().size(); g++) {
      table_chunk.data_[g].Reference(group_chunk.data_[g]);
    }
    table_chunk.SetCardinality(count);

    Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, count};
    group_chunk.Hash(hashes);

    SelectionVector new_sel(count);
    idx_t new_count = 0;
    ht_.FindOrCreateGroups(hashes, table_chunk, addresses, &new_sel, &new_count);

    auto addr_data = FlatVector::GetData<data_ptr_t>(addresses);
    for (idx_t k = 0; k < new_count; k++) {
      const idx_t idx = new_sel.GetIndex(k);
      Store<int64_t>(static_cast<int64_t>(accums_.size()), addr_data[idx] + ordinal_offset_);
      accums_.push_back(MakeAccums(agg_types));
    }
  }

  /** @return The ordinal (accumulator index) stored in the group row at `addr`. */
  auto OrdinalAt(data_ptr_t addr) const -> idx_t { return static_cast<idx_t>(Load<int64_t>(addr + ordinal_offset_)); }
};

}  // namespace

struct HashAggGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  std::unique_ptr<GroupedAggState> state_;  // created by the first Combine (needs the group types)
};

struct HashAggLocalSinkState : LocalSinkState {
  std::unique_ptr<ExpressionExecutor> group_exec_;
  std::unique_ptr<ExpressionExecutor> agg_exec_;
  std::vector<LogicalType> group_types_;
  std::vector<LogicalType> arg_types_;
  std::unique_ptr<GroupedAggState> state_;
};

struct HashAggGlobalSourceState : GlobalSourceState {
  GlobalSinkState *sink_{nullptr};
  std::atomic<idx_t> cursor_{0};
  auto MaxThreads() -> idx_t override { return 1; }
};

auto PhysicalHashAggregate::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<HashAggGlobalSinkState>();
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
  ls->state_ = std::make_unique<GroupedAggState>(ls->group_types_);
  return ls;
}

auto PhysicalHashAggregate::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                 LocalSinkState &lstate) const -> SinkResultType {
  auto &ls = static_cast<HashAggLocalSinkState &>(lstate);
  const idx_t count = input.GetSize();

  DataChunk group_chunk;
  group_chunk.Initialize(ls.group_types_);
  ls.group_exec_->Execute(input, group_chunk);

  DataChunk args;
  args.Initialize(ls.arg_types_);
  ls.agg_exec_->Execute(input, args);

  Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, count};
  ls.state_->AddGroups(group_chunk, agg_types_, addresses);

  // Fold every row into its group's accumulators (the update itself is value-at-a-time for now; the
  // group lookup above is the vectorized part).
  auto addr_data = FlatVector::GetData<data_ptr_t>(addresses);
  auto &accums = ls.state_->accums_;
  for (idx_t i = 0; i < count; i++) {
    auto &group_accums = accums[ls.state_->OrdinalAt(addr_data[i])];
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      group_accums[a].Update(args.GetValue(a, i));
    }
  }
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalHashAggregate::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                    LocalSinkState &lstate) const {
  auto &gs = static_cast<HashAggGlobalSinkState &>(gstate);
  auto &ls = static_cast<HashAggLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  if (gs.state_ == nullptr) {
    // First task in: hand the whole local state over, no per-group merge needed.
    gs.state_ = std::move(ls.state_);
    return;
  }

  // Scan the local groups back out (group columns only — the local ordinal is the scan position) and
  // find-or-create them in the global table, then merge the accumulator sets.
  DataChunk scan_chunk;
  scan_chunk.Initialize(ls.group_types_);
  idx_t offset = 0;
  while (true) {
    const idx_t n = ls.state_->ht_.Scan(offset, scan_chunk);
    if (n == 0) {
      break;
    }
    Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, n};
    gs.state_->AddGroups(scan_chunk, agg_types_, addresses);
    auto addr_data = FlatVector::GetData<data_ptr_t>(addresses);
    for (idx_t j = 0; j < n; j++) {
      auto &global_accums = gs.state_->accums_[gs.state_->OrdinalAt(addr_data[j])];
      auto &local_accums = ls.state_->accums_[offset + j];
      for (idx_t a = 0; a < agg_types_.size(); a++) {
        global_accums[a].Merge(local_accums[a]);
      }
    }
    offset += n;
    scan_chunk.Reset();
  }
}

auto PhysicalHashAggregate::Finalize(ClientContext & /*context*/, GlobalSinkState &gstate, idx_t /*stage*/,
                                     idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  auto &gs = static_cast<HashAggGlobalSinkState &>(gstate);
  if (gs.state_ == nullptr) {
    // No task sank anything (an empty child): an empty table so the source emits nothing.
    std::vector<LogicalType> group_types;
    group_types.reserve(group_bys_.size());
    for (const auto &g : group_bys_) {
      group_types.push_back(g->GetReturnType().GetType());
    }
    gs.state_ = std::make_unique<GroupedAggState>(group_types);
  }
  return SinkFinalizeType::READY;
}

auto PhysicalHashAggregate::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalSourceState> {
  auto src = std::make_unique<HashAggGlobalSourceState>();
  src->sink_ = own_sink_state;
  return src;
}

auto PhysicalHashAggregate::GetData(ExecutionContext & /*context*/, DataChunk &output, GlobalSourceState &gstate,
                                    LocalSourceState & /*lstate*/) const -> SourceResultType {
  auto &src = static_cast<HashAggGlobalSourceState &>(gstate);
  auto &sink = *static_cast<HashAggGlobalSinkState *>(src.sink_);
  auto &state = *sink.state_;
  const idx_t total = state.ht_.Count();
  const idx_t start = src.cursor_.fetch_add(STANDARD_VECTOR_SIZE, std::memory_order_relaxed);
  if (start >= total) {
    return SourceResultType::FINISHED;
  }

  // Scan this batch's group columns into an owned chunk and hand its vectors to the output by
  // reference (the shared data manager keeps them alive). Group g of the scan is ordinal start + j by
  // construction, so the accumulators line up with the scan order.
  const idx_t num_groups = group_bys_.size();
  const auto out_types = output.GetTypes();
  DataChunk group_chunk;
  group_chunk.Initialize(std::vector<LogicalType>(out_types.begin(), out_types.begin() + num_groups));
  const idx_t n = state.ht_.Scan(start, group_chunk, /*copy_strings=*/true);
  for (idx_t g = 0; g < num_groups; g++) {
    output.data_[g].Reference(group_chunk.data_[g]);
  }

  for (idx_t j = 0; j < n; j++) {
    const auto &group_accums = state.accums_[start + j];
    for (idx_t a = 0; a < agg_types_.size(); a++) {
      output.SetValue(num_groups + a, j,
                      group_accums[a].Finalize(output_schema_->GetColumn(num_groups + a).GetType()));
    }
  }
  output.SetCardinality(n);
  return (start + n >= total) ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void PhysicalHashAggregate::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.source_ = this;
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[0]->BuildPipelines(build, builder);
}

}  // namespace bumblebee
