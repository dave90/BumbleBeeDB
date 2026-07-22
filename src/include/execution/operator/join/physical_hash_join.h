//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_hash_join.h
//
// Identification: src/include/execution/operator/join/physical_hash_join.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "binder/table_ref/bound_join_ref.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/physical_operator.h"
#include "type/vector/operations/create_sort_key.h"

namespace bumblebee {

/**
 * @brief An in-memory hash join. Output is `left schema ++ right schema`, per `HashJoinPlanNode`.
 *
 * One operator plays two roles: the **sink** of the build pipeline (it owns the hash table) and a
 * **streaming operator** of the probe pipeline (it reads that same table) — the design's core divergence.
 *
 * The side we build the hash table on depends on the join type, so the **preserved** side is always the
 * streaming probe — which lets unmatched rows be emitted NULL-padded inline, with no build-side match
 * bitmap or extra pipeline (the push engine has no post-pipeline operator flush hook):
 *   - INNER: build the left (child 0), probe the right (child 1).
 *   - LEFT : build the right (child 1), probe the left/preserved (child 0); a probe row with no match is
 *            emitted once with the right columns NULL.
 *
 * The build is parallel in the DuckDB shape: each sink task keys, hashes and scatters its rows into a
 * thread-local `PRLHashTable`; Combine splices the tables block-wise; Finalize runs one directory pass.
 * The probe collects a chunk's (build address, probe row) pairs in probe order — a LEFT no-match row
 * carries a null address — and emits batches as one `Gather` per build column plus a zero-copy `Slice`
 * of the probe columns (the null addresses NULL-pad themselves in the gather).
 *
 * A single probe chunk can match more build rows than fit in one output chunk, so the probe is
 * re-entrant: it parks a cursor in its local state and returns HAVE_MORE_OUTPUT.
 */
class PhysicalHashJoin : public PhysicalOperator {
 public:
  PhysicalHashJoin(SchemaRef output_schema, std::vector<AbstractExpressionRef> left_keys,
                   std::vector<AbstractExpressionRef> right_keys, JoinType join_type, idx_t left_column_count,
                   std::unique_ptr<PhysicalOperator> build, std::unique_ptr<PhysicalOperator> probe)
      : PhysicalOperator(PhysicalOperatorType::HASH_JOIN, std::move(output_schema), probe->estimated_cardinality_),
        left_keys_(std::move(left_keys)),
        right_keys_(std::move(right_keys)),
        join_type_(join_type),
        left_column_count_(left_column_count) {
    key_modifiers_.assign(left_keys_.size(), OrderModifiers(OrderType::ASCENDING));
    children_.push_back(std::move(build));  // child 0 = left input
    children_.push_back(std::move(probe));  // child 1 = right input
  }

  /** @return True for a LEFT OUTER join (preserves the left/child-0 input). */
  auto IsLeftJoin() const -> bool { return join_type_ == JoinType::LEFT; }
  /** @return The child index whose rows are hashed into the build table (left for INNER, right for LEFT). */
  auto BuildChildIdx() const -> idx_t { return IsLeftJoin() ? 1 : 0; }
  /** @return The child index streamed as the probe (right for INNER, the preserved left for LEFT). */
  auto ProbeChildIdx() const -> idx_t { return IsLeftJoin() ? 0 : 1; }
  /** @return The equi-join key expressions evaluated over the build rows. */
  auto BuildKeys() const -> const std::vector<AbstractExpressionRef> & { return IsLeftJoin() ? right_keys_ : left_keys_; }
  /** @return The equi-join key expressions evaluated over the probe rows. */
  auto ProbeKeys() const -> const std::vector<AbstractExpressionRef> & { return IsLeftJoin() ? left_keys_ : right_keys_; }

  // ---- sink role (build the hash table) --------------------------------------
  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;
  auto Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t stage, idx_t task_idx, idx_t task_count) const
      -> SinkFinalizeType override;

  // ---- streaming operator role (probe) ---------------------------------------
  auto IsOperator() const -> bool override { return true; }
  auto GetGlobalOperatorState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalOperatorState> override;
  auto GetLocalOperatorState(ExecutionContext &context) const -> std::unique_ptr<LocalOperatorState> override;
  auto Execute(ExecutionContext &context, DataChunk &input, DataChunk &output, GlobalOperatorState &gstate,
               LocalOperatorState &lstate) const -> OperatorResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  auto ParamsToString() const -> std::string override {
    const char *t = join_type_ == JoinType::INNER ? "Inner" : (join_type_ == JoinType::LEFT ? "Left" : "Outer");
    return "{ type=" + std::string(t) + ", keys=" + std::to_string(left_keys_.size()) + " }";
  }

  std::vector<AbstractExpressionRef> left_keys_;
  std::vector<AbstractExpressionRef> right_keys_;
  JoinType join_type_;
  idx_t left_column_count_;
  std::vector<OrderModifiers> key_modifiers_;
};

}  // namespace bumblebee
