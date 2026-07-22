//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// pipeline.h
//
// Identification: src/include/parallel/pipeline.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "common/config.h"
#include "execution/physical_operator.h"

namespace bumblebee {

class Executor;

/**
 * @brief A `source -> streaming operators -> sink` chain: the unit that gets scheduled.
 *
 * The load-bearing fact: **a pipeline owns no global state.** One `PhysicalHashJoin` is the sink of the
 * build pipeline *and* a streaming operator of the probe pipeline, sharing one hash table; global state
 * is therefore keyed by operator (in the `Executor`'s registry), and a `Pipeline` only caches raw,
 * non-owning pointers into it. Dependencies between pipelines form a DAG driven by atomic counters, so
 * sibling build pipelines run concurrently.
 */
class Pipeline {
 public:
  explicit Pipeline(Executor &executor) : executor_(executor) {}

  /** @brief The task count for this pipeline: the source's hint, collapsed to 1 by any non-parallel role. */
  auto MaxThreads() const -> idx_t;

  auto ToString() const -> std::string;

  Executor &executor_;
  const PhysicalOperator *source_{nullptr};
  std::vector<const PhysicalOperator *> operators_;  // streaming; never materialize
  const PhysicalOperator *sink_{nullptr};

  // Non-owning views into the Executor's operator-indexed state registry.
  GlobalSourceState *source_gstate_{nullptr};
  std::vector<GlobalOperatorState *> operator_gstates_;
  GlobalSinkState *sink_gstate_{nullptr};

  std::vector<Pipeline *> dependencies_;  // must finish before we start
  std::vector<Pipeline *> dependents_;    // released when we finish

  std::atomic<idx_t> dependencies_remaining_{0};
  std::atomic<idx_t> tasks_remaining_{0};
  std::atomic<idx_t> finalize_tasks_remaining_{0};  // finalize-task seam, armed to 1 today
  std::atomic<idx_t> finalize_stage_{0};
  std::atomic<bool> dead_{false};  // an upstream NO_OUTPUT_POSSIBLE: run with an empty source
  std::atomic<bool> stop_{false};  // LIMIT satisfied: all tasks bail
};

}  // namespace bumblebee
