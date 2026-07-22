//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// expression_executor.h
//
// Identification: src/include/execution/expression_executor.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "type/vector/data_chunk.h"
#include "type/vector/selection_vector.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * @brief Evaluates a bound expression tree over a whole `DataChunk` at once — the vectorized eval
 * entry point `AbstractExpression` deliberately deferred (see its class comment).
 *
 * An executor holds one or more *root* expressions and evaluates them against an input chunk. It is
 * cheap to build but owns intermediate `Vector`s during evaluation, so it is **per-thread by
 * construction** — the plan puts one in each operator's `LocalOperatorState` rather than sharing it.
 *
 * Three entry points cover every operator's need:
 *  - `ExecuteExpression(input, out_vec)` — one root → one column (projection column, join/group key).
 *  - `Execute(input, out_chunk)`         — every root → the columns of `out_chunk` (projection).
 *  - `Select(input, sel)`                — a boolean root → the selection vector of matching rows
 *                                          (filter, join residual), returning the match count.
 *
 * Column references and bare projections are **zero-copy**: the result vector references the input
 * column's data manager rather than copying it.
 */
class ExpressionExecutor {
 public:
  ExpressionExecutor() = default;

  /** @brief Build an executor over a single root expression. */
  explicit ExpressionExecutor(const AbstractExpression &expr) { AddExpression(expr); }

  /** @brief Build an executor over several root expressions (e.g. a projection's output columns). */
  explicit ExpressionExecutor(const std::vector<const AbstractExpression *> &exprs) {
    for (const auto *e : exprs) {
      AddExpression(*e);
    }
  }

  /** @brief Append another root expression. */
  void AddExpression(const AbstractExpression &expr) { roots_.push_back(&expr); }

  auto ExpressionCount() const -> idx_t { return roots_.size(); }

  /** @brief Evaluate root `expr_idx` over `input` into `result` (a single vector). */
  void ExecuteExpression(idx_t expr_idx, DataChunk &input, Vector &result);

  /** @brief Evaluate the sole root over `input` into `result`. */
  void ExecuteExpression(DataChunk &input, Vector &result) { ExecuteExpression(0, input, result); }

  /** @brief Evaluate every root into the matching column of `result` (already initialized). */
  void Execute(DataChunk &input, DataChunk &result);

  /**
   * @brief Evaluate the sole (boolean) root over `input` as a filter.
   *
   * @param input The rows to test.
   * @param sel   Filled with the indices of the matching rows (caller sizes it to `input.GetSize()`).
   * @return The number of matching rows.
   */
  auto Select(DataChunk &input, SelectionVector &sel) -> idx_t;

 private:
  /** @brief Recursively evaluate `expr` over `input` (`count` rows), returning its column vector. */
  auto Evaluate(const AbstractExpression &expr, DataChunk &input, idx_t count) -> Vector;

  std::vector<const AbstractExpression *> roots_;
};

}  // namespace bumblebee
