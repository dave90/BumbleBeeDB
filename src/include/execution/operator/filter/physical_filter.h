//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_filter.h
//
// Identification: src/include/execution/operator/filter/physical_filter.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <utility>

#include "execution/expression_executor.h"
#include "execution/physical_operator.h"

namespace bumblebee {

/**
 * @brief A streaming filter: `Select(input) -> sel`, then a **zero-copy** `Slice` of the matched rows.
 *
 * The predicate is compiled into a per-thread `ExpressionExecutor` held in the local operator state, so
 * two tasks never share evaluation scratch.
 */
class PhysicalFilter : public PhysicalOperator {
 public:
  PhysicalFilter(SchemaRef output_schema, AbstractExpressionRef predicate, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::FILTER, std::move(output_schema), child->estimated_cardinality_),
        predicate_(std::move(predicate)) {
    children_.push_back(std::move(child));
  }

  auto IsOperator() const -> bool override { return true; }
  auto PreservesInputColumns() const -> bool override { return true; }

  class LocalState : public LocalOperatorState {
   public:
    std::unique_ptr<ExpressionExecutor> executor_;
    SelectionVector sel_;
  };

  auto GetLocalOperatorState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalOperatorState> override {
    auto ls = std::make_unique<LocalState>();
    ls->executor_ = std::make_unique<ExpressionExecutor>(*predicate_);
    ls->sel_.Initialize(STANDARD_VECTOR_SIZE);
    return ls;
  }

  auto Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &output, GlobalOperatorState & /*g*/,
               LocalOperatorState &lstate) const -> OperatorResultType override {
    auto &ls = static_cast<LocalState &>(lstate);
    const idx_t n = ls.executor_->Select(input, ls.sel_);
    if (n == 0) {
      output.SetCardinality(0);  // swallowed everything; the push loop short-circuits
      return OperatorResultType::NEED_MORE_INPUT;
    }
    output.Slice(input, ls.sel_, n);  // dictionary view over the matched rows — zero copy
    output.SetCardinality(n);
    return OperatorResultType::NEED_MORE_INPUT;
  }

  auto ParamsToString() const -> std::string override { return "{ " + predicate_->ToString() + " }"; }

  AbstractExpressionRef predicate_;
};

}  // namespace bumblebee
