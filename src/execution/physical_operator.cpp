//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_operator.cpp
//
// Identification: src/execution/physical_operator.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/physical_operator.h"

#include <memory>
#include <string>

#include "common/exception.h"
#include "parallel/pipeline_builder.h"

namespace bumblebee {

// ---- streaming operator role (defaults) -----------------------------------------------------------

auto PhysicalOperator::GetGlobalOperatorState(ClientContext & /*context*/, GlobalSinkState * /*own*/) const
    -> std::unique_ptr<GlobalOperatorState> {
  return std::make_unique<GlobalOperatorState>();
}

auto PhysicalOperator::GetLocalOperatorState(ExecutionContext & /*context*/) const
    -> std::unique_ptr<LocalOperatorState> {
  return std::make_unique<LocalOperatorState>();
}

auto PhysicalOperator::Execute(ExecutionContext & /*context*/, DataChunk & /*input*/, DataChunk & /*output*/,
                               GlobalOperatorState & /*gstate*/, LocalOperatorState & /*lstate*/) const
    -> OperatorResultType {
  throw NotImplementedException("operator " + GetName() + " has no Execute (not a streaming operator)");
}

// ---- source role (defaults) -----------------------------------------------------------------------

auto PhysicalOperator::GetGlobalSourceState(ClientContext & /*context*/, GlobalSinkState * /*own*/) const
    -> std::unique_ptr<GlobalSourceState> {
  return std::make_unique<GlobalSourceState>();
}

auto PhysicalOperator::GetLocalSourceState(ExecutionContext & /*context*/, GlobalSourceState & /*gstate*/) const
    -> std::unique_ptr<LocalSourceState> {
  return std::make_unique<LocalSourceState>();
}

auto PhysicalOperator::GetData(ExecutionContext & /*context*/, DataChunk & /*output*/, GlobalSourceState & /*gstate*/,
                               LocalSourceState & /*lstate*/) const -> SourceResultType {
  throw NotImplementedException("operator " + GetName() + " has no GetData (not a source)");
}

// ---- sink role (defaults) -------------------------------------------------------------------------

auto PhysicalOperator::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<GlobalSinkState>();
}

auto PhysicalOperator::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<LocalSinkState>();
}

auto PhysicalOperator::Sink(ExecutionContext & /*context*/, DataChunk & /*input*/, GlobalSinkState & /*gstate*/,
                            LocalSinkState & /*lstate*/) const -> SinkResultType {
  throw NotImplementedException("operator " + GetName() + " has no Sink (not a sink)");
}

void PhysicalOperator::Combine(ExecutionContext & /*context*/, GlobalSinkState & /*gstate*/,
                               LocalSinkState & /*lstate*/) const {}

auto PhysicalOperator::Finalize(ClientContext & /*context*/, GlobalSinkState & /*gstate*/, idx_t /*stage*/,
                                idx_t /*task_idx*/, idx_t /*task_count*/) const -> SinkFinalizeType {
  return SinkFinalizeType::READY;
}

// ---- pipeline construction ------------------------------------------------------------------------

void PhysicalOperator::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  // Streaming default (Filter / Projection / Limit): this operator is a streaming op of the current
  // pipeline; the child continues the same pipeline. Sources, sinks and breakers override this.
  current.operators_.push_back(this);
  children_[0]->BuildPipelines(current, builder);
}

// ---- printing -------------------------------------------------------------------------------------

auto PhysicalOperator::GetName() const -> std::string {
  switch (type_) {
    case PhysicalOperatorType::TABLE_SCAN:
      return "TableScan";
    case PhysicalOperatorType::PARQUET_SCAN:
      return "ParquetScan";
    case PhysicalOperatorType::VALUES:
      return "Values";
    case PhysicalOperatorType::FILTER:
      return "Filter";
    case PhysicalOperatorType::PROJECTION:
      return "Projection";
    case PhysicalOperatorType::LIMIT:
      return "Limit";
    case PhysicalOperatorType::HASH_JOIN:
      return "HashJoin";
    case PhysicalOperatorType::GRACE_HASH_JOIN:
      return "GraceHashJoin";
    case PhysicalOperatorType::NESTED_LOOP_JOIN:
      return "NestedLoopJoin";
    case PhysicalOperatorType::HASH_AGGREGATE:
      return "HashAggregate";
    case PhysicalOperatorType::UNGROUPED_AGGREGATE:
      return "UngroupedAggregate";
    case PhysicalOperatorType::SORT:
      return "Sort";
    case PhysicalOperatorType::EXTERNAL_MERGE_SORT:
      return "ExternalMergeSort";
    case PhysicalOperatorType::TOP_N:
      return "TopN";
    case PhysicalOperatorType::INSERT:
      return "Insert";
    case PhysicalOperatorType::UPDATE:
      return "Update";
    case PhysicalOperatorType::DELETE:
      return "Delete";
    case PhysicalOperatorType::RESULT_COLLECTOR:
      return "ResultCollector";
  }
  return "Unknown";
}

auto PhysicalOperator::ToString(int indent) const -> std::string {
  std::string out(static_cast<size_t>(indent), ' ');
  out += GetName();
  auto params = ParamsToString();
  if (!params.empty()) {
    out += " " + params;
  }
  out += "\n";
  for (const auto &child : children_) {
    out += child->ToString(indent + 2);
  }
  return out;
}

}  // namespace bumblebee
