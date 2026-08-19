//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_heap_test.cpp
//
// Identification: test/unit/storage/table/table_heap_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/table/table_heap.h"

#include <memory>
#include <string>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/config.h"
#include "gtest/gtest.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/table/table_storage.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

static auto MakeSchema() -> SchemaRef {
  std::vector<Column> cols{
      Column("id", LogicalType(LogicalTypeId::INTEGER)),
      Column("name", LogicalType(LogicalTypeId::STRING), VARCHAR_DEFAULT_LENGTH),
      Column("score", LogicalType(LogicalTypeId::DOUBLE)),
  };
  return std::make_shared<Schema>(cols);
}

struct Person {
  int32_t id_;
  std::string name_;
  double score_;
};

static auto MakeChunk(const Schema &schema, const std::vector<Person> &people) -> std::unique_ptr<DataChunk> {
  auto chunk = std::make_unique<DataChunk>();
  chunk->Initialize(schema.GetTypes());
  for (idx_t i = 0; i < people.size(); i++) {
    chunk->SetValue(0, i, Value(people[i].id_));
    chunk->SetValue(1, i, Value(people[i].name_));
    chunk->SetValue(2, i, Value(people[i].score_));
  }
  chunk->SetCardinality(people.size());
  return chunk;
}

TEST(TableHeapTest, VectorizedAppendThenScanRoundTrips) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  std::vector<Person> people{{1, "alice", 90.5}, {2, "bob", 77.25}, {3, "a-considerably-longer-name", 60.0}};
  auto in = MakeChunk(*schema, people);
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(*in, rids);

  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(schema->GetTypes());
  ASSERT_TRUE(scan->Next(out));
  ASSERT_EQ(out.GetSize(), people.size());
  for (idx_t i = 0; i < people.size(); i++) {
    EXPECT_EQ(out.GetValue(0, i), Value(people[i].id_));
    EXPECT_EQ(out.GetValue(1, i).GetString(), people[i].name_);
    EXPECT_EQ(out.GetValue(2, i), Value(people[i].score_));
  }
  DataChunk drained;
  drained.Initialize(schema->GetTypes());
  EXPECT_FALSE(scan->Next(drained));  // one page, then exhausted
}

TEST(TableHeapTest, EstimatedRowCountCountsAppendedRows) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(64, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  EXPECT_EQ(heap.EstimatedRowCount(), 0U);  // empty heap

  // Append enough rows to span several pages (so the estimate must walk the page directory).
  constexpr int kRows = 5000;
  int written = 0;
  while (written < kRows) {
    std::vector<Person> batch;
    const int n = std::min(500, kRows - written);
    for (int i = 0; i < n; i++) {
      batch.push_back({written + i, "person", static_cast<double>(written + i)});
    }
    auto chunk = MakeChunk(*schema, batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap.Append(*chunk, rids);
    written += n;
  }
  // The estimate counts slotted-page tuples: exact here (no deletes), spanning many pages.
  EXPECT_EQ(heap.EstimatedRowCount(), static_cast<idx_t>(kRows));
}

TEST(TableHeapTest, EmptyTableScanEndsImmediately) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(schema->GetTypes());
  EXPECT_FALSE(scan->Next(out));
}

TEST(TableHeapTest, ScanHonorsProjection) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);
  auto in = MakeChunk(*schema, {{7, "g", 1.0}, {8, "h", 2.0}});
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(*in, rids);

  // Project only column 2 (score).
  auto scan = heap.MakeScan({2});
  DataChunk out;
  out.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::DOUBLE)});
  ASSERT_TRUE(scan->Next(out));
  ASSERT_EQ(out.GetSize(), 2U);
  EXPECT_EQ(out.GetValue(0, 0), Value(1.0));
  EXPECT_EQ(out.GetValue(0, 1), Value(2.0));
}

// Bug #8: a scan skips logically deleted rows.
TEST(TableHeapTest, ScanSkipsDeletedRows) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);
  auto in = MakeChunk(*schema, {{1, "a", 1.0}, {2, "b", 2.0}, {3, "c", 3.0}});
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(*in, rids);

  // Delete the middle row (index 1).
  auto rid_data = FlatVector::GetData<int64_t>(rids);
  Vector to_delete{LogicalType{LogicalTypeId::BIGINT}};
  FlatVector::GetData<int64_t>(to_delete)[0] = rid_data[1];
  heap.Delete(to_delete, 1);

  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(schema->GetTypes());
  ASSERT_TRUE(scan->Next(out));
  ASSERT_EQ(out.GetSize(), 2U);  // only rows 1 and 3 survive
  EXPECT_EQ(out.GetValue(0, 0), Value(1));
  EXPECT_EQ(out.GetValue(0, 1), Value(3));
}

TEST(TableHeapTest, FetchGathersByRid) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);
  auto in = MakeChunk(*schema, {{10, "x", 1.5}, {20, "y", 2.5}, {30, "z", 3.5}});
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(*in, rids);

  // Fetch rows 0 and 2 by RID (reverse order).
  Vector to_fetch{LogicalType{LogicalTypeId::BIGINT}};
  auto rid_data = FlatVector::GetData<int64_t>(rids);
  auto fetch_data = FlatVector::GetData<int64_t>(to_fetch);
  fetch_data[0] = rid_data[2];
  fetch_data[1] = rid_data[0];

  DataChunk out;
  out.Initialize(schema->GetTypes());
  heap.Fetch(to_fetch, 2, out);
  ASSERT_EQ(out.GetSize(), 2U);
  EXPECT_EQ(out.GetValue(0, 0), Value(30));
  EXPECT_EQ(out.GetValue(1, 0).GetString(), "z");
  EXPECT_EQ(out.GetValue(0, 1), Value(10));
}

// The DataChunk -> storage update seam: an Update addresses a SUBSET of rows by RID, and only
// those rows may change — the rows not named keep their original values (and their slots/RIDs).
TEST(TableHeapTest, VectorizedUpdateChangesOnlyTargetedRows) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);
  auto in = MakeChunk(*schema, {{1, "a", 1.0}, {2, "b", 2.0}, {3, "c", 3.0}, {4, "d", 4.0}});
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(*in, rids);
  auto rid_data = FlatVector::GetData<int64_t>(rids);

  // Update rows 1 and 3 (0-based) only.
  auto upd = MakeChunk(*schema, {{22, "bee", 22.0}, {44, "dee", 44.0}});
  Vector target{LogicalType{LogicalTypeId::BIGINT}};
  auto *td = FlatVector::GetData<int64_t>(target);
  td[0] = rid_data[1];
  td[1] = rid_data[3];
  heap.Update(target, *upd);

  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(schema->GetTypes());
  ASSERT_TRUE(scan->Next(out));
  ASSERT_EQ(out.GetSize(), 4U);
  // Rows come back in slot (append) order; the untouched rows are unchanged.
  EXPECT_EQ(out.GetValue(0, 0), Value(1));
  EXPECT_EQ(out.GetValue(1, 0).GetString(), "a");
  EXPECT_EQ(out.GetValue(0, 1), Value(22));  // updated
  EXPECT_EQ(out.GetValue(1, 1).GetString(), "bee");
  EXPECT_EQ(out.GetValue(0, 2), Value(3));  // untouched
  EXPECT_EQ(out.GetValue(1, 2).GetString(), "c");
  EXPECT_EQ(out.GetValue(0, 3), Value(44));  // updated
  EXPECT_EQ(out.GetValue(2, 3), Value(44.0));
}

// Updating a row to a LONGER string keeps its RID: the page compacts the row in place.
TEST(TableHeapTest, UpdateGrowsStringInPlaceKeepingRid) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);
  auto in = MakeChunk(*schema, {{1, "x", 1.0}, {2, "y", 2.0}});
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(*in, rids);
  auto rid_data = FlatVector::GetData<int64_t>(rids);
  int64_t rid0 = rid_data[0];

  const std::string longer = "a-much-longer-name-that-exceeds-the-original-inline-size";
  auto upd = MakeChunk(*schema, {{11, longer, 9.0}});
  Vector target{LogicalType{LogicalTypeId::BIGINT}};
  FlatVector::GetData<int64_t>(target)[0] = rid0;
  heap.Update(target, *upd);

  // Fetch by the SAME rid: it must still resolve, now to the grown row.
  Vector fetch{LogicalType{LogicalTypeId::BIGINT}};
  FlatVector::GetData<int64_t>(fetch)[0] = rid0;
  DataChunk out;
  out.Initialize(schema->GetTypes());
  heap.Fetch(fetch, 1, out);
  ASSERT_EQ(out.GetSize(), 1U);
  EXPECT_EQ(out.GetValue(0, 0), Value(11));
  EXPECT_EQ(out.GetValue(1, 0).GetString(), longer);
  EXPECT_EQ(out.GetValue(2, 0), Value(9.0));
}

TEST(TableHeapTest, MultiPageAppendAndScan) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(32, &dm);
  auto schema = MakeSchema();
  TableHeap heap(&bpm, schema);

  // Append enough rows over several chunks to span multiple pages.
  const int total = 2000;
  int next_id = 0;
  for (int batch = 0; batch < 4; batch++) {
    std::vector<Person> people;
    for (int i = 0; i < 500; i++) {
      people.push_back({next_id, "n" + std::to_string(next_id), static_cast<double>(next_id)});
      next_id++;
    }
    auto chunk = MakeChunk(*schema, people);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap.Append(*chunk, rids);
  }

  // Scan everything back and count.
  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(schema->GetTypes());
  int seen = 0;
  int64_t sum_ids = 0;
  while (scan->Next(out)) {
    for (idx_t i = 0; i < out.GetSize(); i++) {
      sum_ids += out.GetValue(0, i).GetAs<int32_t>();
      seen++;
    }
  }
  EXPECT_EQ(seen, total);
  EXPECT_EQ(sum_ids, static_cast<int64_t>(total - 1) * total / 2);
}

}  // namespace bumblebee
