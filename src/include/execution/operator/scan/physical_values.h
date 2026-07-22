//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_values.h
//
// Identification: src/include/execution/operator/scan/physical_values.h
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
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"

namespace bumblebee {

/**
 * @brief A source that emits a literal `VALUES (...), (...)` table by evaluating its constant cells.
 *
 * Single-threaded: one worker walks the rows in `STANDARD_VECTOR_SIZE` batches. Each cell is a (usually
 * constant) expression, evaluated over a one-row dummy input.
 */
class PhysicalValues : public PhysicalOperator {
 public:
  PhysicalValues(SchemaRef output_schema, std::vector<std::vector<AbstractExpressionRef>> values)
      : PhysicalOperator(PhysicalOperatorType::VALUES, std::move(output_schema), values.size()),
        values_(std::move(values)) {}

  auto IsSource() const -> bool override { return true; }

  void BuildPipelines(Pipeline &current, PipelineBuilder & /*builder*/) const override { current.source_ = this; }

  class LocalState : public LocalSourceState {
   public:
    idx_t row_cursor_{0};
  };
  auto GetLocalSourceState(ExecutionContext & /*ctx*/, GlobalSourceState & /*g*/) const
      -> std::unique_ptr<LocalSourceState> override {
    return std::make_unique<LocalState>();
  }

  auto GetData(ExecutionContext & /*ctx*/, DataChunk &output, GlobalSourceState & /*g*/,
               LocalSourceState &lstate) const -> SourceResultType override {
    auto &ls = static_cast<LocalState &>(lstate);
    if (ls.row_cursor_ >= values_.size()) {
      return SourceResultType::FINISHED;
    }
    const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, values_.size() - ls.row_cursor_);

    DataChunk dummy;  // one-row input so constant/arith cells evaluate to a single value
    dummy.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::BOOLEAN)});
    dummy.SetCardinality(1);

    for (idx_t r = 0; r < n; r++) {
      const auto &row = values_[ls.row_cursor_ + r];
      for (idx_t c = 0; c < row.size(); c++) {
        ExpressionExecutor exec(*row[c]);
        Vector cell(output_schema_->GetColumn(c).GetType());
        exec.ExecuteExpression(dummy, cell);
        // The column type is the common supertype across ALL rows, so this row's cell may be
        // narrower (an INT literal in a BIGINT-inferred column) or an untyped NULL — cast the
        // value up to the column type before writing it. A NULL needs no cast: SetValue only
        // clears the validity bit.
        auto v = cell.GetValue(0);
        const auto &target_type = output_schema_->GetColumn(c).GetType();
        output.SetValue(c, r, v.IsNull() || v.GetType() == target_type ? v : v.CastAs(target_type));
      }
    }
    output.SetCardinality(n);
    ls.row_cursor_ += n;
    return ls.row_cursor_ >= values_.size() ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
  }

  std::vector<std::vector<AbstractExpressionRef>> values_;
};

}  // namespace bumblebee
