//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_projection.h
//
// Identification: src/include/execution/operator/projection/physical_projection.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "execution/expression_executor.h"
#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A streaming projection: evaluate each output expression into the corresponding output column.
 *
 * A bare column reference degenerates to a zero-copy `Reference` (the common `SELECT a, b`); an
 * expression column materializes a fresh vector. Evaluation scratch is per-thread (local state).
 */
class PhysicalProjection : public PhysicalOperator {
 public:
  PhysicalProjection(SchemaRef output_schema, std::vector<AbstractExpressionRef> expressions,
                     std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::PROJECTION, std::move(output_schema), child->estimated_cardinality_),
        expressions_(std::move(expressions)) {
    children_.push_back(std::move(child));
  }

  auto IsOperator() const -> bool override { return true; }

  class LocalState : public LocalOperatorState {
   public:
    std::unique_ptr<ExpressionExecutor> executor_;
  };

  auto GetLocalOperatorState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalOperatorState> override {
    auto ls = std::make_unique<LocalState>();
    ls->executor_ = std::make_unique<ExpressionExecutor>();
    for (const auto &e : expressions_) {
      ls->executor_->AddExpression(*e);
    }
    return ls;
  }

  auto Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &output, GlobalOperatorState & /*g*/,
               LocalOperatorState &lstate) const -> OperatorResultType override {
    auto &ls = static_cast<LocalState &>(lstate);
    ls.executor_->Execute(input, output);
    return OperatorResultType::NEED_MORE_INPUT;
  }

  std::vector<AbstractExpressionRef> expressions_;
};

}  // namespace bumblebee
