//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// explain_statement.h
//
// Identification: src/include/binder/statement/explain_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "binder/bound_statement.h"

namespace bumblebee {

/** Which stages of the frontend an EXPLAIN should print. A bitmask. */
enum ExplainOptions : uint8_t {
  /** Print nothing. */
  INVALID = 0,
  /** Print the binder tree. */
  BINDER = 1,
  /** Print the plan. */
  PLANNER = 2,
  /** Print the optimized plan. */
  OPTIMIZER = 4,
  /** Print the output schema. */
  SCHEMA = 8,
  /** Print the physical operator tree (post-lowering). */
  PHYSICAL = 16,
  /** Print the pipeline DAG (post-BuildPipelines). */
  PIPELINES = 32,
  /** Run the query and annotate the physical tree with per-operator profiling. */
  ANALYZE = 64,
};

/**
 * A bound EXPLAIN statement.
 */
class ExplainStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound EXPLAIN.
   *
   * @param statement The statement being explained.
   * @param options The bitmask of stages to print.
   */
  explicit ExplainStatement(std::unique_ptr<BoundStatement> statement, uint8_t options);

  /** The statement being explained. */
  std::unique_ptr<BoundStatement> statement_;

  /** The bitmask of stages to print. */
  uint8_t options_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
