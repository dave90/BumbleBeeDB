//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_operator.h
//
// Identification: src/include/execution/physical_operator.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/config.h"
#include "execution/execution_context.h"

namespace bumblebee {

class ClientContext;
class Pipeline;
class PipelineBuilder;
class DataChunk;

/** A source's `GetData` either produced a chunk and may have more, or is exhausted. */
enum class SourceResultType : uint8_t { HAVE_MORE_OUTPUT, FINISHED };
/** A streaming operator's `Execute`: it wants a fresh input, still owes output, or refuses more (LIMIT). */
enum class OperatorResultType : uint8_t { NEED_MORE_INPUT, HAVE_MORE_OUTPUT, FINISHED };
/** A sink's `Sink`: it can take more input, or is full (a satisfied LIMIT). */
enum class SinkResultType : uint8_t { NEED_MORE_INPUT, FINISHED };
/** A sink's `Finalize`: proceed normally, or the operator is provably empty (skip the dependent scan). */
enum class SinkFinalizeType : uint8_t { READY, NO_OUTPUT_POSSIBLE };

/** The concrete kind of a physical operator (drives lowering, printing and profiling). */
enum class PhysicalOperatorType : uint8_t {
  TABLE_SCAN,
  PARQUET_SCAN,
  PARQUET_INSERT,
  PARQUET_UPDATE,
  PARQUET_DELETE,
  VALUES,
  FILTER,
  PROJECTION,
  LIMIT,
  HASH_JOIN,
  GRACE_HASH_JOIN,
  NESTED_LOOP_JOIN,
  HASH_AGGREGATE,
  UNGROUPED_AGGREGATE,
  SORT,
  EXTERNAL_MERGE_SORT,
  TOP_N,
  INSERT,
  UPDATE,
  DELETE,
  RESULT_COLLECTOR,
};

// -- Per-operator state bases. A concrete operator subclasses the ones it needs. --------------------

/** Global source state: shared by every task of a source's pipeline; self-synchronizing. */
class GlobalSourceState {
 public:
  virtual ~GlobalSourceState() = default;
  /** A hint, read once at scheduling time: how many tasks can usefully pull from this source. */
  virtual auto MaxThreads() -> idx_t { return 1; }
};

/** Local source state: one worker's private cursor into the source. */
class LocalSourceState {
 public:
  virtual ~LocalSourceState() = default;
  /** The global position of the morsel the current chunk came from (sources that advertise
   * SourceProvidesBatchIndex keep it current). An order-dependent sink sorts by it, which
   * reconstructs the serial scan order from a parallel pipeline. */
  idx_t batch_idx_{0};
};

/** Global operator (streaming) state: shared across a pipeline's tasks. */
class GlobalOperatorState {
 public:
  virtual ~GlobalOperatorState() = default;
};

/** Local operator (streaming) state: one worker's private scratch (e.g. an ExpressionExecutor). */
class LocalOperatorState {
 public:
  virtual ~LocalOperatorState() = default;
};

/** Global sink state: the one materialization structure, shared by every task; the pipeline breaker. */
class GlobalSinkState {
 public:
  virtual ~GlobalSinkState() = default;
};

/** Local sink state: one worker's private (lock-free) partial, merged at Combine. */
class LocalSinkState {
 public:
  virtual ~LocalSinkState() = default;
  /** The batch index of the chunk being sunk, copied from the source's local state by the
   * pipeline executor (streaming operators are 1:N within a task, so every derived chunk
   * inherits its source chunk's batch). */
  idx_t batch_idx_{0};
};

/**
 * @brief The base of every physical operator. Every method is `const`; all mutation goes through a
 * state object, so one immutable operator tree can be executed by many pipelines (and re-executed).
 *
 * An operator may play up to three roles — **source**, **streaming operator**, **sink** — each with its
 * own local and global state. That is the single change that lets a hash-join build be an ordinary
 * intermediate operator instead of a lifted-out rule.
 */
class PhysicalOperator {
 public:
  PhysicalOperator(PhysicalOperatorType type, SchemaRef output_schema, idx_t estimated_cardinality)
      : type_(type), output_schema_(std::move(output_schema)), estimated_cardinality_(estimated_cardinality) {}

  virtual ~PhysicalOperator() = default;

  // ---- streaming operator role ------------------------------------------------
  virtual auto IsOperator() const -> bool { return false; }
  virtual auto GetGlobalOperatorState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalOperatorState>;
  virtual auto GetLocalOperatorState(ExecutionContext &context) const -> std::unique_ptr<LocalOperatorState>;
  virtual auto Execute(ExecutionContext &context, DataChunk &input, DataChunk &output, GlobalOperatorState &gstate,
                       LocalOperatorState &lstate) const -> OperatorResultType;
  virtual auto ParallelOperator() const -> bool { return true; }
  /** True when the ROWS this operator passes depend on task interleaving (a streaming LIMIT
   * takes whichever rows arrive first). Order-dependent sinks stay serial below such an operator. */
  virtual auto OperatorOrderDependent() const -> bool { return false; }
  /** True when the output chunk holds exactly the input's columns (a Filter slices them 1:1), so
   * the pipeline executor may carry the source's written-columns hint through this operator. */
  virtual auto PreservesInputColumns() const -> bool { return false; }

  // ---- source role ------------------------------------------------------------
  virtual auto IsSource() const -> bool { return false; }
  virtual auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState>;
  virtual auto GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const
      -> std::unique_ptr<LocalSourceState>;
  virtual auto GetData(ExecutionContext &context, DataChunk &output, GlobalSourceState &gstate,
                       LocalSourceState &lstate) const -> SourceResultType;
  virtual auto IsOrderPreserving() const -> bool { return false; }
  /** True when this source keeps LocalSourceState::batch_idx_ current, i.e. an order-dependent
   * sink downstream can run in parallel and still reconstruct the serial order. */
  virtual auto SourceProvidesBatchIndex() const -> bool { return false; }
  /** The output columns GetData actually writes per chunk, or nullptr for "all of them". A pruned
   * scan writes a handful of a 100-column schema; the pipeline executor then resets only those,
   * and the source establishes the remaining columns (its constant NULLs) once per task. */
  virtual auto SourceWrittenColumns() const -> const std::vector<idx_t> * { return nullptr; }

  // ---- sink role (== pipeline breaker) ----------------------------------------
  virtual auto IsSink() const -> bool { return false; }
  virtual auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState>;
  virtual auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState>;
  virtual auto Sink(ExecutionContext &context, DataChunk &input, GlobalSinkState &gstate, LocalSinkState &lstate) const
      -> SinkResultType;
  virtual void Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const;
  virtual auto Finalize(ClientContext &context, GlobalSinkState &gstate, idx_t stage, idx_t task_idx,
                        idx_t task_count) const -> SinkFinalizeType;
  virtual auto ParallelSink() const -> bool { return true; }
  /** True when this sink's output order must match the serial input order. Such a sink may run
   * in parallel only over a source that provides batch indexes (and with no operator in between
   * that picks rows nondeterministically under parallelism, e.g. a streaming LIMIT). */
  virtual auto SinkOrderDependent() const -> bool { return false; }
  virtual auto FinalizeStageCount(GlobalSinkState &gstate) const -> idx_t { return 1; }
  virtual auto FinalizeMaxThreads(GlobalSinkState &gstate, idx_t stage) const -> idx_t { return 1; }

  /** @brief Grow the pipeline DAG downwards from this operator */
  virtual void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const;

  // ---- printing (EXPLAIN) -----------------------------------------------------
  auto ToString(int indent = 0) const -> std::string;
  virtual auto GetName() const -> std::string;
  virtual auto ParamsToString() const -> std::string { return ""; }

  PhysicalOperatorType type_;
  SchemaRef output_schema_;
  std::vector<std::unique_ptr<PhysicalOperator>> children_;
  idx_t estimated_cardinality_;
  /** Dense id assigned post-order by the plan generator; indexes the Executor's state registry. */
  idx_t id_{0};
  /**
   * The logical `AbstractPlanNode` this operator was lowered from (opaque here), stamped by the plan
   * generator. On a memory-limit overflow an in-memory breaker names it in `MemoryLimitException` so the
   * driver can re-lower just that node to its external variant and retry.
   */
  const void *logical_source_{nullptr};
};

}  // namespace bumblebee
