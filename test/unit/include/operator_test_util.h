//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// operator_test_util.h
//
// Identification: test/unit/include/operator_test_util.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "concurrency/transaction_manager.h"
#include "execution/execution_context.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/physical_operator.h"
#include "main/client_context.h"
#include "parallel/executor.h"
#include "parallel/pipeline.h"
#include "parallel/pipeline_builder.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/**
 * Drive one physical operator directly, without going through SQL.
 *
 * The physical operators were previously reachable only end-to-end through the `.slt` corpus, which
 * proves a query's answer but localises nothing and cannot easily produce the inputs that actually
 * break an operator: an empty build side, an all-NULL key column, a group that spans a chunk
 * boundary, a single-row morsel. This harness feeds an operator hand-built chunks and reads its
 * output back as `Value`s.
 */

/** @brief A row of literal values, the currency this harness deals in. */
using TestRow = std::vector<Value>;

/** @brief Build a schema from (name, type) pairs. */
inline auto MakeSchemaOf(const std::vector<std::pair<const char *, LogicalType>> &cols) -> SchemaRef {
  std::vector<Column> columns;
  columns.reserve(cols.size());
  for (const auto &[name, type] : cols) {
    columns.emplace_back(type.GetTypeId() == LogicalTypeId::STRING ? Column(name, type, VARCHAR_DEFAULT_LENGTH)
                                                                   : Column(name, type));
  }
  return std::make_shared<Schema>(columns);
}

/** @brief A column reference into `schema`, for use as a join key / group / aggregate argument. */
inline auto ColRef(const SchemaRef &schema, uint32_t col_idx, uint32_t tuple_idx = 0) -> AbstractExpressionRef {
  return std::make_shared<ColumnValueExpression>(tuple_idx, col_idx, schema->GetColumn(col_idx));
}

/**
 * @brief A source that replays caller-supplied rows, `chunk_size` at a time.
 *
 * `chunk_size` is deliberately a parameter: emitting the same rows as one chunk and as several
 * is how a chunk-boundary bug shows itself without needing the small-vector build.
 */
class RowScan : public PhysicalOperator {
 public:
  RowScan(SchemaRef schema, std::vector<TestRow> rows, idx_t chunk_size = STANDARD_VECTOR_SIZE)
      : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, std::move(schema), rows.size()),
        rows_(std::move(rows)),
        chunk_size_(chunk_size) {}

  auto IsSource() const -> bool override { return true; }

  void BuildPipelines(Pipeline &current, PipelineBuilder & /*builder*/) const override { current.source_ = this; }

  struct LocalState : LocalSourceState {
    idx_t cursor_{0};
  };

  auto GetLocalSourceState(ExecutionContext & /*ctx*/, GlobalSourceState & /*g*/) const
      -> std::unique_ptr<LocalSourceState> override {
    return std::make_unique<LocalState>();
  }

  auto GetData(ExecutionContext & /*ctx*/, DataChunk &output, GlobalSourceState & /*g*/, LocalSourceState &lstate) const
      -> SourceResultType override {
    auto &ls = static_cast<LocalState &>(lstate);
    if (ls.cursor_ >= rows_.size()) {
      return SourceResultType::FINISHED;
    }
    const idx_t n = std::min<idx_t>(chunk_size_, rows_.size() - ls.cursor_);
    for (idx_t i = 0; i < n; i++) {
      const auto &row = rows_[ls.cursor_ + i];
      for (idx_t c = 0; c < row.size(); c++) {
        output.SetValue(c, i, row[c]);
      }
    }
    output.SetCardinality(n);
    ls.cursor_ += n;
    return ls.cursor_ >= rows_.size() ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
  }

 private:
  std::vector<TestRow> rows_;
  idx_t chunk_size_;
};

/** @brief A sink that materialises every row its child produces, so a test can assert on them. */
class RowCollector : public PhysicalOperator {
 public:
  explicit RowCollector(std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::RESULT_COLLECTOR, child->output_schema_, 0) {
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }

  /** This is the root sink; the child continues the same pipeline (or breaks it itself). Without
   * this override the streaming default would run and the pipeline's `sink_` would stay null. */
  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override {
    current.sink_ = this;
    children_[0]->BuildPipelines(current, builder);
  }

  struct GlobalState : GlobalSinkState {
    std::mutex mu_;
    std::vector<TestRow> rows_;
  };
  struct LocalState : LocalSinkState {
    std::vector<TestRow> rows_;
  };

  auto GetGlobalSinkState(ClientContext & /*ctx*/) const -> std::unique_ptr<GlobalSinkState> override {
    return std::make_unique<GlobalState>();
  }
  auto GetLocalSinkState(ExecutionContext & /*ctx*/) const -> std::unique_ptr<LocalSinkState> override {
    return std::make_unique<LocalState>();
  }

  auto Sink(ExecutionContext & /*ctx*/, DataChunk &input, GlobalSinkState & /*g*/, LocalSinkState &lstate) const
      -> SinkResultType override {
    auto &ls = static_cast<LocalState &>(lstate);
    for (idx_t i = 0; i < input.GetSize(); i++) {
      TestRow row;
      row.reserve(input.ColumnCount());
      for (idx_t c = 0; c < input.ColumnCount(); c++) {
        row.push_back(input.GetValue(c, i));
      }
      ls.rows_.push_back(std::move(row));
    }
    return SinkResultType::NEED_MORE_INPUT;
  }

  void Combine(ExecutionContext & /*ctx*/, GlobalSinkState &gstate, LocalSinkState &lstate) const override {
    auto &gs = static_cast<GlobalState &>(gstate);
    auto &ls = static_cast<LocalState &>(lstate);
    const std::lock_guard<std::mutex> lock(gs.mu_);
    gs.rows_.insert(gs.rows_.end(), std::make_move_iterator(ls.rows_.begin()), std::make_move_iterator(ls.rows_.end()));
  }
};

/** @brief Catalog + transaction manager + buffer pool + client context — what an Executor needs.
 *
 * The buffer pool backs `SpillCollection`, so the out-of-core operators (grace hash join, external
 * merge sort) run against this harness too; shrink `client.mem_` via `SetBudget` to force them to
 * actually spill. `MemoryDiskManager` is fixed-capacity: 4096 pages = 32 MB of spill, far beyond
 * what a unit test writes, and page-id overflow fails loudly (WritePage returns false). */
struct OperatorHarness {
  MemoryDiskManager disk{4096};
  BufferPoolManager bpm{256, &disk};
  /** bpm-backed, so CreateTable() builds a real TableHeap — the persistent-operator tests write
   * through it. The pure streaming-operator tests never touch the catalog and are unaffected. */
  std::unique_ptr<Catalog> catalog{std::make_unique<Catalog>(&bpm)};
  TransactionManager txn_mgr{catalog.get()};
  ClientContext client{*catalog, txn_mgr, &bpm};

  /**
   * @brief Run `root` (which must be a RowCollector) to completion and return the rows it collected.
   *
   * The Executor owns the sink state for the whole query, so the rows stay valid until it is
   * destroyed — hence the copy out.
   */
  auto Run(PhysicalOperator &root) -> std::vector<TestRow> {
    Executor executor(client);
    executor.Initialize(root);
    executor.ExecuteQuery();
    auto *gs = static_cast<RowCollector::GlobalState *>(executor.GetOrCreateSinkState(root));
    return gs->rows_;
  }
};

/** @brief Sort rows so an order-independent operator's output can be compared deterministically. */
inline void SortRows(std::vector<TestRow> &rows) {
  std::sort(rows.begin(), rows.end(), [](const TestRow &a, const TestRow &b) {
    for (size_t i = 0; i < std::min(a.size(), b.size()); i++) {
      if (a[i].IsNull() != b[i].IsNull()) {
        return a[i].IsNull();  // NULLs first, purely for a stable test ordering
      }
      if (a[i].IsNull()) {
        continue;
      }
      const auto sa = a[i].ToString();
      const auto sb = b[i].ToString();
      if (sa != sb) {
        return sa < sb;
      }
    }
    return a.size() < b.size();
  });
}

}  // namespace bumblebee
