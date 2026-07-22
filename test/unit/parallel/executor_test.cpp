//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// executor_test.cpp
//
// Identification: test/unit/parallel/executor_test.cpp
//
// Exercises the pipeline DAG, push loop, operator ordering and breaker dependencies with lightweight
// mock operators — no storage, no expressions.
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <mutex>
#include <vector>

#include "catalog/catalog.h"
#include "concurrency/transaction_manager.h"
#include "execution/physical_operator.h"
#include "gtest/gtest.h"
#include "main/client_context.h"
#include "parallel/executor.h"
#include "parallel/pipeline_builder.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);

auto OneIntSchema() -> SchemaRef {
  return std::make_shared<Schema>(std::vector<Column>{Column("v", kInt)});
}

/** @brief A source that emits `values` split into chunks of `chunk_size`, then FINISHED. */
class MockScan : public PhysicalOperator {
 public:
  MockScan(std::vector<int> values, idx_t chunk_size)
      : PhysicalOperator(PhysicalOperatorType::TABLE_SCAN, OneIntSchema(), values.size()),
        values_(std::move(values)),
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

  auto GetData(ExecutionContext & /*ctx*/, DataChunk &output, GlobalSourceState & /*g*/,
               LocalSourceState &lstate) const -> SourceResultType override {
    auto &ls = static_cast<LocalState &>(lstate);
    if (ls.cursor_ >= values_.size()) {
      return SourceResultType::FINISHED;
    }
    idx_t n = std::min<idx_t>(chunk_size_, values_.size() - ls.cursor_);
    for (idx_t i = 0; i < n; i++) {
      output.SetValue(0, i, Value(values_[ls.cursor_ + i]));
    }
    output.SetCardinality(n);
    ls.cursor_ += n;
    return ls.cursor_ >= values_.size() ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
  }

  std::vector<int> values_;
  idx_t chunk_size_;
};

/** @brief A streaming operator that adds `delta` to column 0 (zero-copy in, fresh out). */
class MockAdd : public PhysicalOperator {
 public:
  MockAdd(int delta, std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::PROJECTION, OneIntSchema(), child->estimated_cardinality_),
        delta_(delta) {
    children_.push_back(std::move(child));
  }

  auto IsOperator() const -> bool override { return true; }

  auto Execute(ExecutionContext & /*ctx*/, DataChunk &input, DataChunk &output, GlobalOperatorState & /*g*/,
               LocalOperatorState & /*l*/) const -> OperatorResultType override {
    idx_t n = input.GetSize();
    for (idx_t i = 0; i < n; i++) {
      output.SetValue(0, i, Value(input.GetValue(0, i).GetAs<int>() + delta_));
    }
    output.SetCardinality(n);
    return OperatorResultType::NEED_MORE_INPUT;
  }

  int delta_;
};

/** @brief A sink that collects every row's column 0 into a shared vector (Combine merges the local). */
class MockCollector : public PhysicalOperator {
 public:
  explicit MockCollector(std::unique_ptr<PhysicalOperator> child)
      : PhysicalOperator(PhysicalOperatorType::RESULT_COLLECTOR, OneIntSchema(), 0) {
    children_.push_back(std::move(child));
  }

  auto IsSink() const -> bool override { return true; }

  struct GlobalState : GlobalSinkState {
    std::mutex mu_;
    std::vector<int> rows_;
  };
  struct LocalState : LocalSinkState {
    std::vector<int> rows_;
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
      ls.rows_.push_back(input.GetValue(0, i).GetAs<int>());
    }
    return SinkResultType::NEED_MORE_INPUT;
  }

  void Combine(ExecutionContext & /*ctx*/, GlobalSinkState &gstate, LocalSinkState &lstate) const override {
    auto &gs = static_cast<GlobalState &>(gstate);
    auto &ls = static_cast<LocalState &>(lstate);
    std::lock_guard lock(gs.mu_);
    gs.rows_.insert(gs.rows_.end(), ls.rows_.begin(), ls.rows_.end());
  }

  void BuildPipelines(Pipeline &current, PipelineBuilder &builder) const override {
    current.sink_ = this;
    children_[0]->BuildPipelines(current, builder);
  }
};

struct Harness {
  std::unique_ptr<Catalog> catalog{std::make_unique<Catalog>()};
  TransactionManager txn_mgr{catalog.get()};
  ClientContext client{*catalog, txn_mgr};
};

}  // namespace

TEST(ExecutorTest, SourceToSinkCollectsEveryRow) {
  Harness h;
  auto collector = std::make_unique<MockCollector>(std::make_unique<MockScan>(std::vector<int>{1, 2, 3, 4, 5}, 2));

  Executor executor(h.client);
  executor.Initialize(*collector);
  executor.ExecuteQuery();

  ASSERT_EQ(executor.Pipelines().size(), 1u);
  auto &gs = static_cast<MockCollector::GlobalState &>(*executor.Pipelines()[0]->sink_gstate_);
  std::vector<int> got = gs.rows_;
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST(ExecutorTest, StreamingOperatorAppliesInSourceToSinkOrder) {
  Harness h;
  // Collector -> Add(+100) -> Add(*implicitly after*) -> Scan.  Two adds test operator ordering.
  auto scan = std::make_unique<MockScan>(std::vector<int>{1, 2, 3}, 2);
  auto add10 = std::make_unique<MockAdd>(10, std::move(scan));
  auto add100 = std::make_unique<MockAdd>(100, std::move(add10));
  auto collector = std::make_unique<MockCollector>(std::move(add100));

  Executor executor(h.client);
  executor.Initialize(*collector);

  // The single pipeline should be: source Scan, operators [Add(10), Add(100)], sink Collector.
  ASSERT_EQ(executor.Pipelines().size(), 1u);
  auto &p = *executor.Pipelines()[0];
  EXPECT_EQ(p.source_->type_, PhysicalOperatorType::TABLE_SCAN);
  ASSERT_EQ(p.operators_.size(), 2u);
  EXPECT_EQ(p.sink_->type_, PhysicalOperatorType::RESULT_COLLECTOR);

  executor.ExecuteQuery();

  auto &gs = static_cast<MockCollector::GlobalState &>(*p.sink_gstate_);
  std::vector<int> got = gs.rows_;
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<int>{111, 112, 113}));  // each +10 then +100
}

}  // namespace bumblebee
