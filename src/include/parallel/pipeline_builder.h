//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// pipeline_builder.h
//
// Identification: src/include/parallel/pipeline_builder.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "parallel/pipeline.h"

namespace bumblebee {

/**
 * @brief Accumulates the pipeline DAG as `PhysicalOperator::BuildPipelines` recurses down the tree.
 *
 * Pipelines are heap-allocated, so growing the internal vector never invalidates a `Pipeline &` held
 * across the recursion — only the owning `unique_ptr` slots move, not the pointees.
 */
class PipelineBuilder {
 public:
  explicit PipelineBuilder(Executor &executor) : executor_(executor) {}

  /** @brief Create the root pipeline (its sink is the root operator, set by that operator itself). */
  auto CreateRootPipeline() -> Pipeline & {
    pipelines_.push_back(std::make_unique<Pipeline>(executor_));
    return *pipelines_.back();
  }

  /**
   * @brief Create a new pipeline whose sink is `sink_op`, as a dependency of `parent`.
   *
   * `parent` must not start until this child finishes: it registers the DAG edge in both directions.
   */
  auto CreateChildPipeline(Pipeline &parent, const PhysicalOperator &sink_op) -> Pipeline & {
    pipelines_.push_back(std::make_unique<Pipeline>(executor_));
    auto &child = *pipelines_.back();
    child.sink_ = &sink_op;
    parent.dependencies_.push_back(&child);
    child.dependents_.push_back(&parent);
    return child;
  }

  /**
   * @brief Record that `dependent` must not start until `dependency` finishes.
   *
   * For sibling pipelines that share a sink and must run in a fixed order — e.g. the grace hash
   * join's two partitioning passes (probe after build), whose order is what tells the shared sink
   * which side a chunk belongs to.
   */
  static void AddDependency(Pipeline &dependent, Pipeline &dependency) {
    dependent.dependencies_.push_back(&dependency);
    dependency.dependents_.push_back(&dependent);
  }

  std::vector<std::unique_ptr<Pipeline>> pipelines_;
  Executor &executor_;
};

}  // namespace bumblebee
