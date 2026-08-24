//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// index_test.cpp
//
// Identification: test/unit/storage/index/index_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <barrier>
#include <memory>
#include <thread>  // NOLINT
#include <vector>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/helper.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/index/generic_key.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

static auto MakeSchema() -> Schema {
  return Schema{std::vector<Column>{
      Column("id", LogicalType(LogicalTypeId::INTEGER)),
      Column("v", LogicalType(LogicalTypeId::INTEGER)),
  }};
}

// Bug #9: SetFromKey rejects a key longer than KeySize (ASan proves no overflow write).
//
// The rejection is a BUMBLEBEE_ASSERT, i.e. a plain `assert`, so it only exists where NDEBUG is
// not set. Release builds compile the check out and the call simply returns, so the death
// expectation is guarded — otherwise this test could never pass in Release. The Debug and ASan
// builds (the ones the bug was found under) still run it.
TEST(GenericKeyTest, SetFromKeyRejectsOverlongKey) {
  GenericKey<8> key;
  std::vector<data_t> ok(8, 1);
  key.SetFromKey(ok.data(), 8);  // exactly fits
#ifndef NDEBUG
  std::vector<data_t> too_long(16, 1);
  EXPECT_DEATH_IF_SUPPORTED(key.SetFromKey(too_long.data(), 16), "");
#endif
}

TEST(GenericComparatorTest, OrdersIntegerKeys) {
  Schema key_schema{std::vector<Column>{Column("k", LogicalType(LogicalTypeId::BIGINT))}};
  GenericComparator<8> cmp(&key_schema);
  GenericKey<8> a;
  GenericKey<8> b;
  a.SetFromInteger(5);
  b.SetFromInteger(9);
  EXPECT_LT(cmp(a, b), 0);
  EXPECT_GT(cmp(b, a), 0);
  EXPECT_EQ(cmp(a, a), 0);
}

// Bug #7: building an index over a table with deleted rows must index only the live rows.
TEST(CatalogIndexTest, CreateIndexSkipsDeletedRowsAndScans) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(32, &dm);
  Catalog catalog(&bpm);
  auto schema = MakeSchema();
  auto table = catalog.CreateTable("t", schema);
  ASSERT_NE(table->storage_, nullptr);

  // Insert ids 0..9 (with v = id * 10).
  DataChunk chunk;
  chunk.Initialize(schema.GetTypes());
  for (int i = 0; i < 10; i++) {
    chunk.SetValue(0, i, Value(i));
    chunk.SetValue(1, i, Value(i * 10));
  }
  chunk.SetCardinality(10);
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  table->storage_->Append(chunk, rids);

  // Delete ids 3 and 7.
  auto rid_data = FlatVector::GetData<int64_t>(rids);
  Vector to_delete{LogicalType{LogicalTypeId::BIGINT}};
  auto del = FlatVector::GetData<int64_t>(to_delete);
  del[0] = rid_data[3];
  del[1] = rid_data[7];
  table->storage_->Delete(to_delete, 2);

  // Build an index on column 0. The key schema is a single INTEGER (4 bytes) -> KeySize 8 is plenty.
  auto index_info = catalog.CreateIndex<8>("t_id_idx", "t", {0});
  ASSERT_NE(index_info, NULL_INDEX_INFO);
  EXPECT_EQ(catalog.GetIndex("t_id_idx", "t"), index_info);

  auto make_key = [&](int32_t id) {
    std::vector<data_t> buf(8, 0);
    Store<int32_t>(id, buf.data() + index_info->key_schema_.GetColumn(0).GetOffset());
    return buf;
  };

  // Live ids resolve to a RID; deleted ids do not.
  for (int i = 0; i < 10; i++) {
    auto buf = make_key(i);
    std::vector<RID> result;
    index_info->index_->ScanKey(buf.data(), index_info->key_schema_.GetInlinedStorageSize(), &result);
    if (i == 3 || i == 7) {
      EXPECT_TRUE(result.empty()) << "deleted id " << i << " should not be indexed";
    } else {
      ASSERT_EQ(result.size(), 1U) << "live id " << i << " should be indexed";
      EXPECT_EQ(result[0].Get(), rid_data[i]);
    }
  }
}

// A composite index over two columns exercises the vectorized key gather's multi-column path end to
// end: every (id, v) pair must round-trip through the packed key layout and resolve to its RID.
TEST(CatalogIndexTest, CompositeKeyIndexBuildsAndScans) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(32, &dm);
  Catalog catalog(&bpm);
  auto schema = MakeSchema();
  auto table = catalog.CreateTable("t", schema);
  ASSERT_NE(table->storage_, nullptr);

  DataChunk chunk;
  chunk.Initialize(schema.GetTypes());
  const int n = 40;
  for (int i = 0; i < n; i++) {
    chunk.SetValue(0, i, Value(i));
    chunk.SetValue(1, i, Value(i * 10));
  }
  chunk.SetCardinality(n);
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  table->storage_->Append(chunk, rids);
  auto rid_data = FlatVector::GetData<int64_t>(rids);

  // Composite key on (id, v). Two INTEGERs -> 8 bytes of key content -> KeySize 16 is ample.
  auto index_info = catalog.CreateIndex<16>("t_id_v_idx", "t", {0, 1});
  ASSERT_NE(index_info, NULL_INDEX_INFO);

  const auto &ks = index_info->key_schema_;
  auto make_key = [&](int32_t id, int32_t v) {
    std::vector<data_t> buf(16, 0);
    Store<int32_t>(id, buf.data() + ks.GetColumn(0).GetOffset());
    Store<int32_t>(v, buf.data() + ks.GetColumn(1).GetOffset());
    return buf;
  };

  for (int i = 0; i < n; i++) {
    auto buf = make_key(i, i * 10);
    std::vector<RID> result;
    index_info->index_->ScanKey(buf.data(), ks.GetInlinedStorageSize(), &result);
    ASSERT_EQ(result.size(), 1U) << "composite key (" << i << "," << i * 10 << ") should resolve";
    EXPECT_EQ(result[0].Get(), rid_data[i]);
  }

  // A composite key that never existed (right id, wrong v) resolves to nothing.
  auto miss = make_key(5, 999);
  std::vector<RID> none;
  index_info->index_->ScanKey(miss.data(), ks.GetInlinedStorageSize(), &none);
  EXPECT_TRUE(none.empty());
}

TEST(CatalogIndexTest, ConcurrentUniqueInsertHasExactlyOnePublisher) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(32, &dm);
  Catalog catalog(&bpm);
  auto table = catalog.CreateTable("t", MakeSchema());
  ASSERT_NE(table->storage_, nullptr);
  auto index_info = catalog.CreateIndex<8>("t_id_idx", "t", {0});
  ASSERT_NE(index_info, NULL_INDEX_INFO);

  std::vector<data_t> key(8, 0);
  Store<int32_t>(42, key.data() + index_info->key_schema_.GetColumn(0).GetOffset());

  constexpr int kThreads = 8;
  std::barrier start(kThreads);
  std::atomic<int> publishers{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread = 0; thread < kThreads; thread++) {
    threads.emplace_back([&, thread] {
      start.arrive_and_wait();
      if (index_info->index_->InsertEntry(key.data(), index_info->key_schema_.GetInlinedStorageSize(),
                                          RID(100 + thread, 0))) {
        publishers.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(publishers.load(), 1);
  std::vector<RID> result;
  index_info->index_->ScanKey(key.data(), index_info->key_schema_.GetInlinedStorageSize(), &result);
  EXPECT_EQ(result.size(), 1U);
}

TEST(CatalogIndexTest, UniqueIndexBuildRejectsDuplicateKeys) {
  MemoryDiskManager dm(1024);
  BufferPoolManager bpm(32, &dm);
  Catalog catalog(&bpm);
  auto schema = MakeSchema();
  auto table = catalog.CreateTable("t", schema);
  ASSERT_NE(table->storage_, nullptr);

  DataChunk chunk;
  chunk.Initialize(schema.GetTypes());
  chunk.SetValue(0, 0, Value(7));
  chunk.SetValue(1, 0, Value(10));
  chunk.SetValue(0, 1, Value(7));
  chunk.SetValue(1, 1, Value(20));
  chunk.SetCardinality(2);
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  table->storage_->Append(chunk, rids);

  EXPECT_THROW(static_cast<void>(catalog.CreateIndex<8>("t_id_idx", "t", {0})), ExecutionException);
  EXPECT_EQ(catalog.GetIndex("t_id_idx", "t"), NULL_INDEX_INFO);
}

}  // namespace bumblebee
