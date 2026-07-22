//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// operator_profile.h
//
// Identification: src/include/parallel/operator_profile.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace bumblebee {

/** Whether the per-operator profiler is compiled in. Kept a constant so it can be switched off. */
static constexpr bool kProfilingEnabled = true;

/** The role/phase a `ProfileScope` times — selects which counter of an `OperatorProfile` it bumps. */
enum class Phase : uint8_t { Execute, Source, Sink, Combine, Finalize };

/**
 * @brief POD counters for one operator — cardinality flow plus nanoseconds spent in each phase.
 *
 * One instance per operator per task; the `Executor` sums them element-wise (by the dense operator id)
 * into a single per-query profile. Refactored from BumbleBee's pointer-keyed `PhysicalAtomProfiler`.
 */
struct OperatorProfile {
  uint64_t rows_in{0};
  uint64_t rows_out{0};
  uint64_t chunks{0};
  uint64_t exec_ns{0};
  uint64_t source_ns{0};
  uint64_t sink_ns{0};
  uint64_t combine_ns{0};
  uint64_t finalize_ns{0};

  void operator+=(const OperatorProfile &o) {
    rows_in += o.rows_in;
    rows_out += o.rows_out;
    chunks += o.chunks;
    exec_ns += o.exec_ns;
    source_ns += o.source_ns;
    sink_ns += o.sink_ns;
    combine_ns += o.combine_ns;
    finalize_ns += o.finalize_ns;
  }
};

}  // namespace bumblebee
