//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// top_n_heap_test.cpp
//
// Identification: test/unit/execution/top_n_heap_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/sort/top_n_heap.h"

#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

using OptInt = std::optional<int32_t>;

constexpr LogicalTypeId INT = LogicalTypeId::INTEGER;
constexpr LogicalTypeId STR = LogicalTypeId::STRING;
const OrderModifiers ASC{OrderType::ASCENDING};
const OrderModifiers DESC{OrderType::DESCENDING};

/** A two-column chunk (INTEGER, STRING) where row i's string tags its int, to catch misalignment. */
static auto MakeChunk(const std::vector<OptInt> &ints) -> DataChunk {
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{INT, STR});
  for (idx_t i = 0; i < ints.size(); i++) {
    if (ints[i].has_value()) {
      chunk.SetValue(0, i, Value(*ints[i]));
      chunk.SetValue(1, i, Value("tag" + std::to_string(*ints[i])));
    } else {
      chunk.SetValue(0, i, Value::Null(INT));
      chunk.SetValue(1, i, Value("tagnull"));
    }
  }
  chunk.SetCardinality(ints.size());
  return chunk;
}

/** Sink `input` ordering by its column 0, the way the operator would feed the heap. */
static void SinkByCol0(TopNHeap &heap, DataChunk &input) {
  DataChunk keys;
  keys.InitAndReference(input, {0});
  heap.Sink(input, keys);
}

/** Drain the finalized heap and return (col0, col1) of every emitted row, in emission order. */
static auto Drain(TopNHeap &heap) -> std::vector<std::pair<OptInt, std::string>> {
  std::vector<std::pair<OptInt, std::string>> rows;
  DataChunk out;
  out.Initialize(std::vector<LogicalType>{INT, STR});
  for (idx_t pos = 0;; pos += STANDARD_VECTOR_SIZE) {
    out.Reset();
    const idx_t got = heap.GetData(out, pos);
    if (got == 0) {
      break;
    }
    for (idx_t i = 0; i < got; i++) {
      auto v = out.GetValue(0, i);
      rows.emplace_back(v.IsNull() ? OptInt{} : OptInt{v.GetAs<int32_t>()}, out.GetValue(1, i).GetString());
    }
  }
  return rows;
}

static auto IntHeap(const OrderModifiers &mod, idx_t limit) -> TopNHeap {
  return TopNHeap{std::vector<LogicalType>{INT, STR}, std::vector<LogicalType>{INT}, {mod}, limit};
}

TEST(TopNHeapTest, KeepsSmallestAscendingAcrossChunks) {
  auto heap = IntHeap(ASC, 5);
  auto c1 = MakeChunk({30, 7, 45, 12});
  auto c2 = MakeChunk({3, 99, 21});
  auto c3 = MakeChunk({8, 60, 1, 33});
  SinkByCol0(heap, c1);
  SinkByCol0(heap, c2);
  SinkByCol0(heap, c3);
  heap.Finalize();

  auto rows = Drain(heap);
  ASSERT_EQ(rows.size(), 5U);
  const std::vector<int32_t> expected{1, 3, 7, 8, 12};
  for (idx_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(rows[i].first, OptInt{expected[i]});
    EXPECT_EQ(rows[i].second, "tag" + std::to_string(expected[i]));  // payload stays row-aligned
  }
}

TEST(TopNHeapTest, KeepsLargestDescending) {
  auto heap = IntHeap(DESC, 3);
  auto c1 = MakeChunk({30, 7, 45, 12});
  auto c2 = MakeChunk({3, 99, 21});
  SinkByCol0(heap, c1);
  SinkByCol0(heap, c2);
  heap.Finalize();

  auto rows = Drain(heap);
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0].first, OptInt{99});
  EXPECT_EQ(rows[1].first, OptInt{45});
  EXPECT_EQ(rows[2].first, OptInt{30});
}

// Once the heap is full, a chunk that is entirely worse on the first (integer) key is pruned before
// its sort keys are built; a mixed chunk is pruned row-wise. Either way the result must be exact.
TEST(TopNHeapTest, PrefilterPrunesWorseRows) {
  auto heap = IntHeap(ASC, 2);
  auto good = MakeChunk({5, 1, 9});
  SinkByCol0(heap, good);  // heap now holds {1, 5}

  auto all_worse = MakeChunk({100, 200, 300});
  SinkByCol0(heap, all_worse);  // whole chunk pruned

  auto mixed = MakeChunk({50, 2, 70, std::nullopt});  // only 2 (and the NULL candidate) survive the prefilter
  SinkByCol0(heap, mixed);

  heap.Finalize();
  auto rows = Drain(heap);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].first, OptInt{1});
  EXPECT_EQ(rows[1].first, OptInt{2});
}

TEST(TopNHeapTest, NullsLastAscendingFirstDescending) {
  {
    auto heap = IntHeap(ASC, 3);
    auto c = MakeChunk({std::nullopt, 4, 2});
    SinkByCol0(heap, c);
    heap.Finalize();
    auto rows = Drain(heap);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].first, OptInt{2});
    EXPECT_EQ(rows[1].first, OptInt{4});
    EXPECT_FALSE(rows[2].first.has_value());
  }
  {
    auto heap = IntHeap(DESC, 3);
    auto c = MakeChunk({std::nullopt, 4, 2});
    SinkByCol0(heap, c);
    heap.Finalize();
    auto rows = Drain(heap);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_FALSE(rows[0].first.has_value());
    EXPECT_EQ(rows[1].first, OptInt{4});
    EXPECT_EQ(rows[2].first, OptInt{2});
  }
}

// String keys take the non-prefiltered path; long strings also exercise the key StringHeap.
TEST(TopNHeapTest, StringKeysWithTies) {
  TopNHeap heap{std::vector<LogicalType>{STR, INT}, std::vector<LogicalType>{STR}, {ASC}, 3};
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{STR, INT});
  const std::vector<std::string> names{"pear", "apple", "apple", "banana, a long string that is not inlined", "cherry"};
  for (idx_t i = 0; i < names.size(); i++) {
    chunk.SetValue(0, i, Value(names[i]));
    chunk.SetValue(1, i, Value(static_cast<int32_t>(i)));
  }
  chunk.SetCardinality(names.size());
  DataChunk keys;
  keys.InitAndReference(chunk, {0});
  heap.Sink(chunk, keys);
  heap.Finalize();

  DataChunk out;
  out.Initialize(std::vector<LogicalType>{STR, INT});
  ASSERT_EQ(heap.GetData(out, 0), 3U);
  EXPECT_EQ(out.GetValue(0, 0).GetString(), "apple");
  EXPECT_EQ(out.GetValue(0, 1).GetString(), "apple");
  EXPECT_EQ(out.GetValue(0, 2).GetString(), "banana, a long string that is not inlined");
}

TEST(TopNHeapTest, MultiKeySecondaryDescending) {
  // ORDER BY a ASC, b DESC over (a, b) pairs encoded as one payload... use two key columns.
  TopNHeap heap{std::vector<LogicalType>{INT, INT}, std::vector<LogicalType>{INT, INT}, {ASC, DESC}, 4};
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{INT, INT});
  const std::vector<std::pair<int32_t, int32_t>> vals{{2, 1}, {1, 5}, {2, 9}, {1, 3}, {3, 0}};
  for (idx_t i = 0; i < vals.size(); i++) {
    chunk.SetValue(0, i, Value(vals[i].first));
    chunk.SetValue(1, i, Value(vals[i].second));
  }
  chunk.SetCardinality(vals.size());
  DataChunk keys;
  keys.InitAndReference(chunk, {0, 1});
  heap.Sink(chunk, keys);
  heap.Finalize();

  DataChunk out;
  out.Initialize(std::vector<LogicalType>{INT, INT});
  ASSERT_EQ(heap.GetData(out, 0), 4U);
  const std::vector<std::pair<int32_t, int32_t>> expected{{1, 5}, {1, 3}, {2, 9}, {2, 1}};
  for (idx_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(out.GetValue(0, i).GetAs<int32_t>(), expected[i].first);
    EXPECT_EQ(out.GetValue(1, i).GetAs<int32_t>(), expected[i].second);
  }
}

// Feed ever-improving rows so every row enters the heap and the dead-row payload grows past the
// compaction threshold: Reduce must fire (repeatedly) without losing or misaligning the survivors.
TEST(TopNHeapTest, ReduceCompactsUnderSustainedEviction) {
  constexpr int32_t TOTAL = 5000;
  auto heap = IntHeap(ASC, 3);
  std::vector<OptInt> batch;
  for (int32_t v = TOTAL; v > 0; v--) {  // descending: each row beats the previous ones
    batch.emplace_back(v);
    if (batch.size() == STANDARD_VECTOR_SIZE || v == 1) {
      auto chunk = MakeChunk(batch);
      SinkByCol0(heap, chunk);
      batch.clear();
    }
  }
  heap.Finalize();

  auto rows = Drain(heap);
  ASSERT_EQ(rows.size(), 3U);
  for (int32_t i = 0; i < 3; i++) {
    EXPECT_EQ(rows[i].first, OptInt{i + 1});
    EXPECT_EQ(rows[i].second, "tag" + std::to_string(i + 1));
  }
}

TEST(TopNHeapTest, CombineMergesLocalHeaps) {
  auto global = IntHeap(ASC, 4);
  auto local1 = IntHeap(ASC, 4);
  auto local2 = IntHeap(ASC, 4);
  auto local3 = IntHeap(ASC, 4);  // stays empty

  auto c1 = MakeChunk({40, 10, 70});
  auto c2 = MakeChunk({5, 55, 20});
  SinkByCol0(local1, c1);
  SinkByCol0(local2, c2);
  global.Combine(local1);
  global.Combine(local2);
  global.Combine(local3);
  global.Finalize();

  auto rows = Drain(global);
  ASSERT_EQ(rows.size(), 4U);
  const std::vector<int32_t> expected{5, 10, 20, 40};
  for (idx_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(rows[i].first, OptInt{expected[i]});
    EXPECT_EQ(rows[i].second, "tag" + std::to_string(expected[i]));
  }
}

TEST(TopNHeapTest, LimitZeroKeepsNothing) {
  auto heap = IntHeap(ASC, 0);
  auto c = MakeChunk({3, 1, 2});
  SinkByCol0(heap, c);
  heap.Finalize();
  EXPECT_EQ(heap.GetSize(), 0U);
  DataChunk out;
  out.Initialize(std::vector<LogicalType>{INT, STR});
  EXPECT_EQ(heap.GetData(out, 0), 0U);
}

TEST(TopNHeapTest, LimitExceedsInputReturnsAllSorted) {
  auto heap = IntHeap(ASC, 100);
  auto c = MakeChunk({3, 1, 2});
  SinkByCol0(heap, c);
  heap.Finalize();
  auto rows = Drain(heap);
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0].first, OptInt{1});
  EXPECT_EQ(rows[1].first, OptInt{2});
  EXPECT_EQ(rows[2].first, OptInt{3});
}

// A limit above STANDARD_VECTOR_SIZE forces GetData to page through the sorted payload.
TEST(TopNHeapTest, LargeLimitPagesThroughGetData) {
  const idx_t limit = 2 * STANDARD_VECTOR_SIZE + 500;
  constexpr int32_t TOTAL = 4000;
  auto heap = IntHeap(ASC, limit);
  std::vector<OptInt> batch;
  for (int32_t v = 0; v < TOTAL; v++) {
    batch.emplace_back((v * 7919) % TOTAL);  // a permutation of 0..TOTAL-1
    if (batch.size() == STANDARD_VECTOR_SIZE || v == TOTAL - 1) {
      auto chunk = MakeChunk(batch);
      SinkByCol0(heap, chunk);
      batch.clear();
    }
  }
  heap.Finalize();

  auto rows = Drain(heap);
  ASSERT_EQ(rows.size(), limit);
  for (idx_t i = 0; i < limit; i++) {
    EXPECT_EQ(rows[i].first, OptInt{static_cast<int32_t>(i)});
  }
}

}  // namespace bumblebee
