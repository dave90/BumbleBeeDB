//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// executor.h
//
// Identification: src/include/parallel/executor.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <vector>

#include "common/config.h"
#include "execution/physical_operator.h"
#include "main/client_context.h"
#include "parallel/operator_profile.h"
#include "parallel/pipeline.h"
#include "parallel/task.h"
#include "parallel/task_scheduler.h"
#include "parallel/thread_profiler.h"

namespace bumblebee {

/**
 * @brief Owns a query's per-operator global state, builds its pipeline DAG, and runs it.
 *
 * Global state is keyed by the dense `PhysicalOperator::id_` in three flat vectors — not a hash map and
 * not a `mutable` member on the operator — so "exactly one global state per operator, shared by every
 * pipeline that touches it" is a structural invariant. The registry is touched only
 * during `Initialize`, on the client thread; the hot path reads the cached raw pointers on each Pipeline.
 *
 * This version runs single-threaded (the client thread drains each pipeline in dependency order). The
 * counter-based parallel scheduler slots in behind the same `Pipeline`/`PipelineExecutor` without
 * changing them.
 */
class Executor {
 public:
  explicit Executor(ClientContext &context) : context_(context) {}

  /** @brief Number the operator tree, build the pipeline DAG, and wire every global-state pointer. */
  void Initialize(PhysicalOperator &root);

  /** @brief Run every pipeline in dependency order. Rethrows the first execution error, if any. */
  void ExecuteQuery();

  auto Context() -> ClientContext & { return context_; }
  auto MaxThreads() const -> idx_t { return context_.config_.max_threads_; }
  auto NumOperators() const -> idx_t { return num_operators_; }
  auto HasError() const -> bool { return has_error_.load(std::memory_order_acquire); }

  /**
   * @brief The most tasks this query can have runnable at any one instant.
   *
   * Bounds the worker pool: pipelines on one dependency chain run one after another, so only
   * mutually independent ones add up. See the definition for why it never under-counts.
   */
  auto PeakTaskDemand() const -> idx_t;

  /** @brief Latch the first execution exception and flag the query as failed. */
  void PushError(std::exception_ptr e);

  // ---- the operator-indexed global-state registry (memoized; one state per operator) --------------
  auto GetOrCreateSinkState(const PhysicalOperator &op) -> GlobalSinkState *;
  auto GetOrCreateSourceState(const PhysicalOperator &op) -> GlobalSourceState *;
  auto GetOrCreateOperatorState(const PhysicalOperator &op) -> GlobalOperatorState *;

  /** @brief Fold one finished task's per-operator profile into the query-wide profile (thread-safe). */
  void MergeThreadProfiler(const ThreadProfiler &tp);
  auto QueryProfile() const -> const std::vector<OperatorProfile> & { return query_profile_; }

  // ---- the counter-based scheduling protocol, driven by PipelineTask -----------------------------
  /** @brief Arm `p.tasks_remaining_` and append `p`'s tasks to `out` (before they can be observed). */
  void CreateTasks(Pipeline &p, std::vector<TaskRef> &out);
  /** @brief The last task of `p` ran: finalize its sink and release its dependents. */
  void RunFinalize(Pipeline &p);
  /** @brief The last task of `p` ran but the query has errored: count it done, release nothing. */
  void AbortPipeline(Pipeline &p);
  /** @brief One task finished: when the last one does, the query is over — wake the client. */
  void TaskFinished();

  auto Pipelines() const -> const std::vector<std::unique_ptr<Pipeline>> & { return pipelines_; }

  /** @brief Render the pipeline DAG in dependency order (for EXPLAIN (pipelines) / \pipelines). */
  auto PipelinesToString() const -> std::string;

  /** @brief Render the physical tree annotated with the merged per-operator profile (EXPLAIN ANALYZE). */
  auto AnalyzeToString(const PhysicalOperator &op, int indent = 0) const -> std::string;

 private:
  /** @brief Release `p`'s dependents; if `no_output_possible`, mark them dead first. */
  void ReleaseDependents(Pipeline &p, bool no_output_possible);

  ClientContext &context_;
  std::vector<std::unique_ptr<Pipeline>> pipelines_;
  idx_t num_operators_{0};

  std::vector<std::unique_ptr<GlobalSinkState>> sink_states_;
  std::vector<std::unique_ptr<GlobalSourceState>> source_states_;
  std::vector<std::unique_ptr<GlobalOperatorState>> operator_states_;

  std::vector<OperatorProfile> query_profile_;
  std::mutex profile_mutex_;

  TaskScheduler scheduler_;
  std::atomic<idx_t> active_tasks_{0};  // outstanding tasks across the whole query
  std::atomic<bool> query_done_{false};

  std::atomic<bool> has_error_{false};
  std::exception_ptr first_error_;
  std::mutex error_mutex_;
};

}  // namespace bumblebee
