//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_nested_loop_join.cpp
//
// Identification: src/execution/operator/join/physical_nested_loop_join.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/join/physical_nested_loop_join.h"

#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "execution/expression_executor.h"
#include "execution/expressions/column_value_expression.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

namespace {

/**
 * @brief Rewrite the two-sided join predicate onto the combined `left ++ right` chunk.
 *
 * The vectorized `ExpressionExecutor` evaluates over ONE chunk and reads a column reference by its
 * `col_idx` alone, so a right-side reference (`tuple_idx == 1`) is remapped to `left_cols + col_idx`;
 * a left-side reference keeps its index. Every other node is cloned with its children rewritten.
 */
auto RemapOntoCombined(const AbstractExpression &expr, idx_t left_cols) -> AbstractExpressionRef {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(&expr)) {
    const auto idx = col->GetTupleIdx() == 0 ? col->GetColIdx() : left_cols + col->GetColIdx();
    return std::make_shared<ColumnValueExpression>(0, static_cast<uint32_t>(idx), col->GetReturnType());
  }
  std::vector<AbstractExpressionRef> children;
  children.reserve(expr.children_.size());
  for (const auto &child : expr.children_) {
    children.push_back(RemapOntoCombined(*child, left_cols));
  }
  return expr.CloneWithChildren(std::move(children));
}

}  // namespace

struct NljGlobalSinkState : GlobalSinkState {
  std::mutex mu_;
  ChunkCollection inner_;
};

struct NljLocalSinkState : LocalSinkState {
  ChunkCollection inner_;
};

struct NljGlobalOperatorState : GlobalOperatorState {
  GlobalSinkState *sink_{nullptr};
};

struct NljLocalOperatorState : LocalOperatorState {
  AbstractExpressionRef combined_predicate_;  // the join predicate over the left ++ right chunk
  std::unique_ptr<ExpressionExecutor> pred_exec_;

  bool processing_{false};  // mid-way through the current outer chunk
  idx_t inner_idx_{0};      // the inner chunk currently joined against
  // The matches of (outer chunk x current inner chunk): parallel outer/inner row selections.
  std::vector<sel_t> pairs_outer_;
  std::vector<sel_t> pairs_inner_;
  idx_t pair_cursor_{0};
  bool pairs_ready_{false};
  std::vector<uint8_t> outer_matched_;  // LEFT: which outer rows matched anything, across inner chunks
  bool left_done_{false};               // LEFT: the NULL-padded leftovers were emitted
};

auto PhysicalNestedLoopJoin::GetGlobalSinkState(ClientContext & /*context*/) const -> std::unique_ptr<GlobalSinkState> {
  return std::make_unique<NljGlobalSinkState>();
}

auto PhysicalNestedLoopJoin::GetLocalSinkState(ExecutionContext & /*context*/) const -> std::unique_ptr<LocalSinkState> {
  return std::make_unique<NljLocalSinkState>();
}

auto PhysicalNestedLoopJoin::Sink(ExecutionContext & /*context*/, DataChunk &input, GlobalSinkState & /*gstate*/,
                                  LocalSinkState &lstate) const -> SinkResultType {
  static_cast<NljLocalSinkState &>(lstate).inner_.Append(input);
  return SinkResultType::NEED_MORE_INPUT;
}

void PhysicalNestedLoopJoin::Combine(ExecutionContext & /*context*/, GlobalSinkState &gstate,
                                     LocalSinkState &lstate) const {
  auto &gs = static_cast<NljGlobalSinkState &>(gstate);
  auto &ls = static_cast<NljLocalSinkState &>(lstate);
  std::lock_guard lock(gs.mu_);
  gs.inner_.Append(ls.inner_);
}

auto PhysicalNestedLoopJoin::GetGlobalOperatorState(ClientContext & /*context*/, GlobalSinkState *own_sink_state) const
    -> std::unique_ptr<GlobalOperatorState> {
  auto gop = std::make_unique<NljGlobalOperatorState>();
  gop->sink_ = own_sink_state;
  return gop;
}

auto PhysicalNestedLoopJoin::GetLocalOperatorState(ExecutionContext & /*context*/) const
    -> std::unique_ptr<LocalOperatorState> {
  auto ls = std::make_unique<NljLocalOperatorState>();
  ls->combined_predicate_ = RemapOntoCombined(*predicate_, left_column_count_);
  ls->pred_exec_ = std::make_unique<ExpressionExecutor>(*ls->combined_predicate_);
  return ls;
}

auto PhysicalNestedLoopJoin::Execute(ExecutionContext & /*context*/, DataChunk &input, DataChunk &output,
                                     GlobalOperatorState &gstate, LocalOperatorState &lstate) const
    -> OperatorResultType {
  auto &gop = static_cast<NljGlobalOperatorState &>(gstate);
  auto &inner = static_cast<NljGlobalSinkState *>(gop.sink_)->inner_;
  auto &ls = static_cast<NljLocalOperatorState &>(lstate);
  const bool left = join_type_ == JoinType::LEFT;
  const idx_t left_cols = left_column_count_;
  const idx_t right_cols = output.ColumnCount() - left_cols;
  const idx_t count = input.GetSize();
  const auto out_types = output.GetTypes();

  if (!ls.processing_) {
    ls.processing_ = true;
    ls.inner_idx_ = 0;
    ls.pairs_ready_ = false;
    ls.left_done_ = false;
    if (left) {
      ls.outer_matched_.assign(count, 0);
    }
  }

  while (true) {
    if (!ls.pairs_ready_) {
      if (ls.inner_idx_ >= inner.ChunkCount()) {
        // Every inner chunk is joined. LEFT emits the never-matched outer rows once, NULL-padded.
        ls.processing_ = false;
        if (left && !ls.left_done_) {
          ls.left_done_ = true;
          // An OWNING selection: the output dictionary shares its refcounted buffer, which must
          // outlive this call (the downstream operators read the output after we return).
          SelectionVector sel(count);
          idx_t unmatched = 0;
          for (idx_t i = 0; i < count; i++) {
            if (ls.outer_matched_[i] == 0) {
              sel.SetIndex(unmatched++, i);
            }
          }
          if (unmatched > 0) {
            output.Slice(input, sel, unmatched, 0);
            for (idx_t c = 0; c < right_cols; c++) {
              output.data_[left_cols + c].Reference(Value::Null(out_types[left_cols + c]));
            }
            output.SetCardinality(unmatched);
            return OperatorResultType::NEED_MORE_INPUT;
          }
        }
        output.SetCardinality(0);
        return OperatorResultType::NEED_MORE_INPUT;
      }

      // Match the whole outer chunk against this inner chunk: per outer row, broadcast its left
      // columns as constants over the inner rows and run one vectorized Select on the predicate.
      auto &rchunk = inner.GetChunk(ls.inner_idx_);
      const idx_t m = rchunk.GetSize();
      ls.pairs_outer_.clear();
      ls.pairs_inner_.clear();
      ls.pair_cursor_ = 0;

      DataChunk combined;
      combined.InitializeEmpty(out_types);
      for (idx_t c = 0; c < right_cols; c++) {
        combined.data_[left_cols + c].Reference(rchunk.data_[c]);
      }
      combined.SetCardinality(m);

      SelectionVector matches(m);
      for (idx_t i = 0; i < count; i++) {
        for (idx_t c = 0; c < left_cols; c++) {
          combined.data_[c].Reference(input.GetValue(c, i));
        }
        const idx_t n = ls.pred_exec_->Select(combined, matches);
        for (idx_t j = 0; j < n; j++) {
          ls.pairs_outer_.push_back(static_cast<sel_t>(i));
          ls.pairs_inner_.push_back(static_cast<sel_t>(matches.GetIndex(j)));
        }
        if (left && n > 0) {
          ls.outer_matched_[i] = 1;
        }
      }
      ls.pairs_ready_ = true;
      if (ls.pairs_outer_.empty()) {
        ls.pairs_ready_ = false;
        ls.inner_idx_++;
        continue;  // nothing matched this inner chunk; try the next
      }
    }

    // Emit the next batch of this inner chunk's matches: both sides are zero-copy slices.
    const idx_t total = ls.pairs_outer_.size();
    const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, total - ls.pair_cursor_);
    SelectionVector outer_sel(ls.pairs_outer_.data() + ls.pair_cursor_);
    SelectionVector inner_sel(ls.pairs_inner_.data() + ls.pair_cursor_);
    output.Slice(input, outer_sel, n, 0);
    output.Slice(inner.GetChunk(ls.inner_idx_), inner_sel, n, left_cols);
    output.SetCardinality(n);
    ls.pair_cursor_ += n;

    if (ls.pair_cursor_ >= total) {
      ls.pairs_ready_ = false;
      ls.inner_idx_++;
      // More work left for this outer chunk (another inner chunk, or the LEFT leftovers)?
      if (ls.inner_idx_ < inner.ChunkCount() || (left && !ls.left_done_)) {
        return OperatorResultType::HAVE_MORE_OUTPUT;
      }
      ls.processing_ = false;
      return OperatorResultType::NEED_MORE_INPUT;
    }
    return OperatorResultType::HAVE_MORE_OUTPUT;
  }
}

void PhysicalNestedLoopJoin::BuildPipelines(Pipeline &current, PipelineBuilder &builder) const {
  current.operators_.push_back(this);
  children_[0]->BuildPipelines(current, builder);  // outer (left) streams through this operator
  auto &build = builder.CreateChildPipeline(current, *this);
  children_[1]->BuildPipelines(build, builder);  // inner (right) is materialized into the sink
}

}  // namespace bumblebee
