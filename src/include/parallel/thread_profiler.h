//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// thread_profiler.h
//
// Identification: src/include/parallel/thread_profiler.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>
#include <vector>

#include "common/config.h"
#include "parallel/operator_profile.h"

namespace bumblebee {

/**
 * @brief A task's private per-operator profile table, indexed by the dense `PhysicalOperator::id_`.
 *
 * One per task, on the `ThreadContext`. A task touches only its own table, so there is no lock and no
 * pointer-keyed map — just a flat vector in the same index space as the `Executor`'s state registry.
 */
class ThreadProfiler {
 public:
  explicit ThreadProfiler(idx_t num_ops) : profiles_(num_ops) {}

  auto operator[](idx_t op_id) -> OperatorProfile & { return profiles_[op_id]; }
  auto Get(idx_t op_id) const -> const OperatorProfile & { return profiles_[op_id]; }
  auto Size() const -> idx_t { return profiles_.size(); }

  std::vector<OperatorProfile> profiles_;
};

/**
 * @brief RAII timer: adds the elapsed nanoseconds of its lexical block to one operator's phase counter.
 *
 * Replaces BumbleBee's stateful start/end pairs — start and end cannot be mispaired, and it nests.
 */
class ProfileScope {
 public:
  ProfileScope(ThreadProfiler &profiler, idx_t op_id, Phase phase)
      : profiler_(profiler), op_id_(op_id), phase_(phase), start_(steady_clock_t::now()) {}

  ~ProfileScope() {
    if constexpr (!kProfilingEnabled) {
      return;
    }
    const auto ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock_t::now() - start_).count());
    auto &p = profiler_[op_id_];
    switch (phase_) {
      case Phase::Execute:
        p.exec_ns += ns;
        break;
      case Phase::Source:
        p.source_ns += ns;
        break;
      case Phase::Sink:
        p.sink_ns += ns;
        break;
      case Phase::Combine:
        p.combine_ns += ns;
        break;
      case Phase::Finalize:
        p.finalize_ns += ns;
        break;
    }
  }

  ProfileScope(const ProfileScope &) = delete;
  auto operator=(const ProfileScope &) -> ProfileScope & = delete;

 private:
  ThreadProfiler &profiler_;
  idx_t op_id_;
  Phase phase_;
  time_point_t start_;
};

}  // namespace bumblebee
