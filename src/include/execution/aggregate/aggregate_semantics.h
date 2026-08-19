//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// aggregate_semantics.h
//
// Identification: src/include/execution/aggregate/aggregate_semantics.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "execution/plans/aggregation_plan.h"

namespace bumblebee {

// The two rules every aggregate path has to agree on, stated once.
//
// There are three implementations of aggregation in the engine — the scalar AggregateAccumulator
// (ungrouped), the row-scatter kernels (grouped hot loop), and the partition merge — and each of
// them accumulates, merges and finalizes. Restating "AVG is SUM until finalize" or open-coding the
// extreme fold in each one is how the three drift apart.
//
// Everything here is constexpr and branch-free at the call site, so the hot kernels pay nothing.

/**
 * @brief Does this aggregate accumulate a running sum?
 *
 * AVG carries exactly the same state as SUM — a running value plus a count — and differs only at
 * finalize, where it divides one by the other. Accumulate and merge treat them identically.
 */
[[nodiscard]] constexpr auto IsSumLike(AggregationType type) -> bool {
  return type == AggregationType::SumAggregate || type == AggregationType::AvgAggregate;
}

/** @brief Does this aggregate keep a running extreme (MIN/MAX) rather than an accumulation? */
[[nodiscard]] constexpr auto IsExtremeAgg(AggregationType type) -> bool {
  return type == AggregationType::MinAggregate || type == AggregationType::MaxAggregate;
}

/**
 * @brief Should `candidate` replace the current extreme `cur`? Direction known at compile time.
 *
 * Separate from FoldExtreme because taking the new extreme is not always a plain assignment: the
 * string MIN/MAX path has to copy the bytes into its own heap, so it needs the decision first.
 *
 * @tparam MIN True for MIN, false for MAX.
 */
template <bool MIN, class T>
[[nodiscard]] constexpr auto TakesExtreme(const T &cur, const T &candidate) -> bool {
  if constexpr (MIN) {
    return candidate < cur;
  } else {
    return candidate > cur;
  }
}

/** @brief Should `candidate` replace `cur`? Direction taken from `type` (MIN, else MAX). */
template <class T>
[[nodiscard]] constexpr auto TakesExtreme(AggregationType type, const T &cur, const T &candidate) -> bool {
  return type == AggregationType::MinAggregate ? TakesExtreme<true>(cur, candidate)
                                               : TakesExtreme<false>(cur, candidate);
}

/**
 * @brief Fold `candidate` into the running extreme `cur`. Direction known at compile time.
 *
 * Keeps `cur` on a tie and on NaN, exactly like the `candidate < cur ? candidate : cur` this
 * replaced — the comparison is unchanged, so IEEE behaviour is unchanged.
 */
template <bool MIN, class T>
[[nodiscard]] constexpr auto FoldExtreme(T cur, T candidate) -> T {
  return TakesExtreme<MIN>(cur, candidate) ? candidate : cur;
}

/** @brief Fold `candidate` into `cur`, with the direction taken from `type` (MIN, else MAX). */
template <class T>
[[nodiscard]] constexpr auto FoldExtreme(AggregationType type, T cur, T candidate) -> T {
  return TakesExtreme(type, cur, candidate) ? candidate : cur;
}

}  // namespace bumblebee
