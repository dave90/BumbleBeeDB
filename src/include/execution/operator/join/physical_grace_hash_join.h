//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_grace_hash_join.h
//
// Identification: src/include/execution/operator/join/physical_grace_hash_join.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "catalog/schema.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/physical_operator.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

/**
 * @brief The out-of-core hash join, grace style: partition BOTH sides to disk, then join the pairs.
 *
 * Three phases over three pipelines (output = left ++ right, INNER and LEFT):
 *
 * 1. **Partition build** (parallel sink): every build row routes to one of `GH_PARTITION_COUNT`
 *    spill partitions by the low bits of its key hash. Tasks route their own chunks concurrently and
 *    append straight into the shared partitions (`SpillCollection::Append` synchronizes internally).
 * 2. **Partition probe** (parallel sink, a sibling pipeline ordered AFTER phase 1 via a pipeline
 *    dependency): every probe row routes to its partition with the same bits — no joining yet. The
 *    shared sink state needs no side tag: the phase, flipped by the first `Finalize`, says which
 *    side is sinking.
 * 3. **Join** (parallel source): tasks claim partition pairs from a shared queue — load build
 *    partition p into a `PRLHashTable` ONCE, stream probe partition p through it, emit. An oversized
 *    pair re-partitions BOTH sides with the next 6 hash bits; a pair that will not split (a hot key)
 *    falls back to a role-flipped block nested loop. Each build partition is therefore scanned and
 *    indexed exactly once, and each probe row is written and read exactly once.
 *
 * Like the in-memory join, the **preserved** side of a LEFT join is always the probe:
 *   - INNER: build the left (child 0), probe the right (child 1).
 *   - LEFT : build the right (child 1), probe the left/preserved (child 0). A probe row that matches
 *     nothing in its pair is emitted once with the build columns NULL; NULL-keyed probe rows go to a
 *     dedicated spill during phase 2 and are emitted NULL-padded first.
 */
class PhysicalGraceHashJoin : public PhysicalOperator {
 public:
  PhysicalGraceHashJoin(SchemaRef output_schema, std::vector<AbstractExpressionRef> left_keys,
                        std::vector<AbstractExpressionRef> right_keys, JoinType join_type, idx_t left_column_count,
                        std::unique_ptr<PhysicalOperator> left, std::unique_ptr<PhysicalOperator> right);

  /** @return True for a LEFT OUTER join (preserves the left/child-0 input). */
  auto IsLeftJoin() const -> bool { return join_type_ == JoinType::LEFT; }
  /** @return The child index whose rows are spilled to build partitions (left for INNER, right for LEFT). */
  auto BuildChildIdx() const -> idx_t { return IsLeftJoin() ? 1 : 0; }
  /** @return The child index streamed as the probe (right for INNER, the preserved left for LEFT). */
  auto ProbeChildIdx() const -> idx_t { return IsLeftJoin() ? 0 : 1; }
  /** @return The equi-join key expressions evaluated over the build rows. */
  auto BuildKeys() const -> const std::vector<AbstractExpressionRef> & { return IsLeftJoin() ? right_keys_ : left_keys_; }
  /** @return The equi-join key expressions evaluated over the probe rows. */
  auto ProbeKeys() const -> const std::vector<AbstractExpressionRef> & { return IsLeftJoin() ? left_keys_ : right_keys_; }

  auto IsSink() const -> bool override { return true; }
  auto ParallelSink() const -> bool override { return true; }  // tasks append to the shared partitions
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;
  auto Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t stage, idx_t task_idx, idx_t task_count) const
      -> SinkFinalizeType override;

  auto IsSource() const -> bool override { return true; }
  auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState> override;
  auto GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const
      -> std::unique_ptr<LocalSourceState> override;
  auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate,
               LocalSourceState &lstate) const -> SourceResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  auto ParamsToString() const -> std::string override {
    return "{ type=" + std::string(join_type_ == JoinType::INNER ? "Inner" : "Left") + ", partitioned }";
  }

  std::vector<AbstractExpressionRef> left_keys_;
  std::vector<AbstractExpressionRef> right_keys_;
  JoinType join_type_;
  idx_t left_column_count_;
  std::vector<OrderModifiers> key_modifiers_;
  SchemaRef build_schema_;  // the build child's output schema (what the build partitions store)
  SchemaRef probe_schema_;  // the probe child's output schema (what the probe partitions store)
};

}  // namespace bumblebee
