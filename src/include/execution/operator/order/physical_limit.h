//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_limit.h
//
// Identification: src/include/execution/operator/order/physical_limit.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A streaming LIMIT: pass rows through until a shared counter reaches `limit`, then FINISH.
 *
 * Zero-copy — the passed-through rows reference the input; only the final partial batch is truncated by
 * setting a smaller cardinality.
 */
class PhysicalLimit : public PhysicalOperator {
 public:
  PhysicalLimit(SchemaRef output_schema, std::size_t limit, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::LIMIT, std::move(output_schema), child->estimated_cardinality_),
        limit_(limit) {
    children_.push_back(std::move(child));
  }

  auto IsOperator() const -> bool override { return true; }
  // Which rows the shared counter admits depends on task interleaving: an order-dependent sink
  // below a parallel limit could not reconstruct the serial result, so it stays serial.
  auto OperatorOrderDependent() const -> bool override { return true; }

  class GlobalState : public GlobalOperatorState {
   public:
    std::atomic<idx_t> count{0};
  };

  auto GetGlobalOperatorState(ClientContext & /*context*/, GlobalSinkState * /*own*/) const
      -> std::unique_ptr<GlobalOperatorState> override {
    return std::make_unique<GlobalState>();
  }

  auto Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &output, GlobalOperatorState &gstate,
               LocalOperatorState & /*lstate*/) const -> OperatorResultType override {
    auto &gs = static_cast<GlobalState &>(gstate);
    // Reserve `take` rows atomically so two parallel tasks can never over-produce past the limit.
    idx_t already = gs.count.load(std::memory_order_relaxed);
    idx_t take;
    do {
      if (already >= limit_) {
        return OperatorResultType::FINISHED;
      }
      take = std::min<idx_t>(input.GetSize(), limit_ - already);
    } while (!gs.count.compare_exchange_weak(already, already + take, std::memory_order_relaxed));
    output.Reference(input);          // rows are in order; the first `take` are the ones we keep
    output.SetCardinality(take);
    // Produce this (possibly final) batch with NEED_MORE_INPUT so it is sinked; the next call, once the
    // count has reached the limit, returns FINISHED with no output. (Matches the push-loop convention.)
    return OperatorResultType::NEED_MORE_INPUT;
  }

  auto ParamsToString() const -> std::string override { return "{ n=" + std::to_string(limit_) + " }"; }

  std::size_t limit_;
};

}  // namespace bumblebee
