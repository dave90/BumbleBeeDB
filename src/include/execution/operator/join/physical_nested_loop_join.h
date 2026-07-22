//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_nested_loop_join.h
//
// Identification: src/include/execution/operator/join/physical_nested_loop_join.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>

#include "binder/table_ref/bound_join_ref.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A nested-loop join for predicates a hash join can't handle (non-equi, cross product).
 *
 * The inner side (right, child 1) is materialized into the sink; the outer side (left, child 0) streams
 * through this operator. The two-sided predicate is rewritten once onto the combined `left ++ right`
 * chunk (right column references shifted by the left column count), then each outer row is joined
 * against an inner chunk with ONE vectorized `ExpressionExecutor::Select` — its left columns broadcast
 * as constants over the inner rows — and matches are emitted as zero-copy slices of both sides.
 * Output = left ++ right; the (inner chunk, match cursor) state makes the emission re-entrant.
 *
 * Handles INNER and LEFT. Because the preserved (left) side is the streaming outer side, a LEFT join
 * emits the outer rows that matched no inner row once at the end of the chunk, right columns NULL.
 */
class PhysicalNestedLoopJoin : public PhysicalOperator {
 public:
  PhysicalNestedLoopJoin(SchemaRef output_schema, AbstractExpressionRef predicate, JoinType join_type,
                         idx_t left_column_count, std::unique_ptr<PhysicalOperator> outer,
                         std::unique_ptr<PhysicalOperator> inner)
      : PhysicalOperator(PhysicalOperatorType::NESTED_LOOP_JOIN, std::move(output_schema),
                         outer->estimated_cardinality_),
        predicate_(std::move(predicate)),
        join_type_(join_type),
        left_column_count_(left_column_count) {
    children_.push_back(std::move(outer));  // child 0 = outer (left), streams
    children_.push_back(std::move(inner));  // child 1 = inner (right), materialized
  }

  auto IsSink() const -> bool override { return true; }
  auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState> override;
  auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState> override;
  auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType override;
  void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const override;

  auto IsOperator() const -> bool override { return true; }
  auto GetGlobalOperatorState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalOperatorState> override;
  auto GetLocalOperatorState(ExecutionContext &context) const -> std::unique_ptr<LocalOperatorState> override;
  auto Execute(ExecutionContext &context, DataChunk &input, DataChunk &output, GlobalOperatorState &gstate,
               LocalOperatorState &lstate) const -> OperatorResultType override;

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override;

  AbstractExpressionRef predicate_;
  JoinType join_type_;
  idx_t left_column_count_;
};

}  // namespace bumblebee
