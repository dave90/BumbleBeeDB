//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// persistence_test.cpp
//
// Identification: test/unit/persistence_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "database.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/helper.h"
#include "concurrency/transaction_manager.h"
#include "gtest/gtest.h"
#include "storage/mvcc/mvcc.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

namespace {

auto TempDbPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / name;
}

auto TwoColSchema() -> Schema {
  return Schema{std::vector<Column>{
      Column("id", LogicalType(LogicalTypeId::INTEGER)),
      Column("name", LogicalType(LogicalTypeId::STRING), 128),
  }};
}

auto TypesOf(const Schema &s) -> std::vector<LogicalType> {
  std::vector<LogicalType> t;
  for (const auto &c : s.GetColumns()) {
    t.push_back(c.GetType());
  }
  return t;
}

void AppendRows(TableStorage &storage, const Schema &schema,
                const std::vector<std::pair<int32_t, std::string>> &rows) {
  DataChunk chunk;
  chunk.Initialize(TypesOf(schema));
  for (idx_t i = 0; i < rows.size(); i++) {
    chunk.SetValue(0, i, Value(rows[i].first));
    chunk.SetValue(1, i, Value(rows[i].second));
  }
  chunk.SetCardinality(rows.size());
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  storage.Append(chunk, rids);
}

auto ScanAll(TableStorage &storage, const Schema &schema) -> std::vector<std::pair<int32_t, std::string>> {
  auto scan = storage.MakeScan();
  DataChunk out;
  out.Initialize(TypesOf(schema));
  std::vector<std::pair<int32_t, std::string>> rows;
  while (scan->Next(out)) {
    for (idx_t i = 0; i < out.GetSize(); i++) {
      rows.emplace_back(out.GetValue(0, i).GetAs<int32_t>(), out.GetValue(1, i).GetString());
    }
  }
  return rows;
}

}  // namespace

// Tables, schemas, and rows written by one Database instance are recovered by a fresh instance on the
// same file — the core durability proof.
TEST(PersistenceTest, TablesAndRowsSurviveRestart) {
  auto path = TempDbPath("bbdb_persist_tables.db");
  std::filesystem::remove(path);
  {
    Database db(path, 32);
    auto ti = db.GetCatalog().CreateTable("t", TwoColSchema());
    ASSERT_NE(ti, nullptr);
    AppendRows(*ti->storage_, ti->schema_, {{1, "alice"}, {2, "bob"}, {3, "carol"}});
    db.Close();
  }
  {
    Database db(path, 32);
    auto ti = db.GetCatalog().GetTable("t");
    ASSERT_NE(ti, nullptr) << "the table must be recovered";
    ASSERT_EQ(ti->schema_.GetColumnCount(), 2U);
    EXPECT_EQ(ti->schema_.GetColumn(1).GetName(), "name");
    auto rows = ScanAll(*ti->storage_, ti->schema_);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0], std::make_pair(1, std::string("alice")));
    EXPECT_EQ(rows[1], std::make_pair(2, std::string("bob")));
    EXPECT_EQ(rows[2], std::make_pair(3, std::string("carol")));
  }
  std::filesystem::remove(path);
}

// After reopen, new allocation does not collide with recovered pages: a second table's rows and the
// first table's rows coexist intact.
TEST(PersistenceTest, AllocatorDoesNotCollideAfterRestart) {
  auto path = TempDbPath("bbdb_persist_alloc.db");
  std::filesystem::remove(path);
  {
    Database db(path, 32);
    auto t1 = db.GetCatalog().CreateTable("t1", TwoColSchema());
    AppendRows(*t1->storage_, t1->schema_, {{1, "one"}, {2, "two"}});
    db.Close();
  }
  {
    Database db(path, 32);
    auto t2 = db.GetCatalog().CreateTable("t2", TwoColSchema());  // fresh pages, must not overwrite t1
    ASSERT_NE(t2, nullptr);
    AppendRows(*t2->storage_, t2->schema_, {{9, "nine"}});

    auto r1 = ScanAll(*db.GetCatalog().GetTable("t1")->storage_, TwoColSchema());
    ASSERT_EQ(r1.size(), 2U);
    EXPECT_EQ(r1[0].second, "one");
    auto r2 = ScanAll(*t2->storage_, TwoColSchema());
    ASSERT_EQ(r2.size(), 1U);
    EXPECT_EQ(r2[0].second, "nine");
    db.Close();
  }
  std::filesystem::remove(path);
}

// The allocator free list is persisted: a page freed before Close is reused (not leaked) after reopen.
TEST(PersistenceTest, FreeListSurvivesRestart) {
  auto path = TempDbPath("bbdb_persist_freelist.db");
  std::filesystem::remove(path);
  page_id_t freed = INVALID_PAGE_ID;
  {
    Database db(path, 32);
    freed = db.GetBufferPool().NewPage();   // a scratch page
    db.GetBufferPool().DeletePage(freed);   // reclaim it
    db.Close();
  }
  {
    Database db(path, 32);
    auto reused = db.GetBufferPool().NewPage();
    EXPECT_EQ(reused, freed) << "the reopened allocator reuses the persisted freed id rather than growing";
    db.Close();
  }
  std::filesystem::remove(path);
}

// A B+ tree index survives restart AND is fully functional afterwards: it finds pre-restart keys, and
// live insert / delete / scan on the reopened tree still work (proving open-existing, not just readable).
TEST(PersistenceTest, IndexSurvivesRestartAndIsFunctional) {
  auto path = TempDbPath("bbdb_persist_index.db");
  std::filesystem::remove(path);

  std::vector<int64_t> rid_by_id(20, 0);
  {
    Database db(path, 32);
    auto ti = db.GetCatalog().CreateTable("t", TwoColSchema());
    // Insert ids 0..9 and capture their RIDs, then build an index on column 0.
    DataChunk chunk;
    chunk.Initialize(TypesOf(ti->schema_));
    for (int i = 0; i < 10; i++) {
      chunk.SetValue(0, i, Value(i));
      chunk.SetValue(1, i, Value(std::string("n") + std::to_string(i)));
    }
    chunk.SetCardinality(10);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    ti->storage_->Append(chunk, rids);
    auto rid_data = FlatVector::GetData<int64_t>(rids);
    for (int i = 0; i < 10; i++) {
      rid_by_id[i] = rid_data[i];
    }
    ASSERT_NE(db.GetCatalog().CreateIndex<8>("t_id_idx", "t", {0}), NULL_INDEX_INFO);
    db.Close();
  }
  {
    Database db(path, 32);
    auto info = db.GetCatalog().GetIndex("t_id_idx", "t");
    ASSERT_NE(info, NULL_INDEX_INFO) << "the index must be recovered";
    const auto &ks = info->key_schema_;
    auto make_key = [&](int32_t id) {
      std::vector<data_t> buf(8, 0);
      Store<int32_t>(id, buf.data() + ks.GetColumn(0).GetOffset());
      return buf;
    };
    auto lookup = [&](int32_t id) {
      std::vector<RID> res;
      info->index_->ScanKey(make_key(id).data(), ks.GetInlinedStorageSize(), &res);
      return res;
    };

    // Pre-restart keys resolve to their original RIDs.
    for (int i = 0; i < 10; i++) {
      auto res = lookup(i);
      ASSERT_EQ(res.size(), 1U) << "recovered key " << i;
      EXPECT_EQ(res[0].Get(), rid_by_id[i]);
    }

    // The reopened tree is live: insert a new key, delete an existing one, and both take effect.
    info->index_->InsertEntry(make_key(100).data(), ks.GetInlinedStorageSize(), RID(123, 4));
    EXPECT_EQ(lookup(100).size(), 1U) << "insert on the reopened tree works";
    info->index_->DeleteEntry(make_key(5).data(), ks.GetInlinedStorageSize(), RID(0, 0));
    EXPECT_TRUE(lookup(5).empty()) << "delete on the reopened tree works";
    EXPECT_EQ(lookup(6).size(), 1U) << "an untouched key still resolves";
    db.Close();
  }
  std::filesystem::remove(path);
}

// A catalog too large for a single page spills onto an overflow chain and is recovered in full. 300
// tables (each with a schema) far exceed one 8 KiB page, forcing several chained catalog pages.
TEST(PersistenceTest, LargeCatalogSpillsToOverflowChain) {
  auto path = TempDbPath("bbdb_persist_overflow.db");
  std::filesystem::remove(path);
  constexpr int kTables = 300;
  {
    Database db(path, 64);
    for (int i = 0; i < kTables; i++) {
      auto ti = db.GetCatalog().CreateTable("table_number_" + std::to_string(i), TwoColSchema());
      ASSERT_NE(ti, nullptr);
      AppendRows(*ti->storage_, ti->schema_, {{i, "row-" + std::to_string(i)}});
    }
    db.Close();
  }
  {
    Database db(path, 64);
    // Every table (in the middle and at the ends of the chain) is recovered with its row.
    for (int i = 0; i < kTables; i++) {
      auto ti = db.GetCatalog().GetTable("table_number_" + std::to_string(i));
      ASSERT_NE(ti, nullptr) << "table " << i << " lost across the overflow chain";
      auto rows = ScanAll(*ti->storage_, ti->schema_);
      ASSERT_EQ(rows.size(), 1U);
      EXPECT_EQ(rows[0], std::make_pair(i, "row-" + std::to_string(i)));
    }
    // The catalog is still usable after reopen: adding one more table round-trips through a re-grown chain.
    ASSERT_NE(db.GetCatalog().CreateTable("one_more", TwoColSchema()), nullptr);
    db.Close();
  }
  {
    Database db(path, 64);
    EXPECT_NE(db.GetCatalog().GetTable("one_more"), nullptr);
    EXPECT_NE(db.GetCatalog().GetTable("table_number_0"), nullptr);
    EXPECT_NE(db.GetCatalog().GetTable("table_number_299"), nullptr);
  }
  std::filesystem::remove(path);
}

// A table large enough to span several pages survives restart with all rows intact and in order.
TEST(PersistenceTest, MultiPageTableSurvivesRestart) {
  auto path = TempDbPath("bbdb_persist_multipage.db");
  std::filesystem::remove(path);
  constexpr int kRows = 600;
  {
    Database db(path, 32);
    auto ti = db.GetCatalog().CreateTable("big", TwoColSchema());
    std::vector<std::pair<int32_t, std::string>> rows;
    rows.reserve(kRows);
    for (int i = 0; i < kRows; i++) {
      rows.emplace_back(i, "r" + std::to_string(i));
    }
    AppendRows(*ti->storage_, ti->schema_, rows);
    ASSERT_NE(static_cast<TableHeap *>(ti->storage_.get())->GetFirstPageId(),
              static_cast<TableHeap *>(ti->storage_.get())->GetLastPageId())
        << "the table must span multiple pages";
    db.Close();
  }
  {
    Database db(path, 32);
    auto ti = db.GetCatalog().GetTable("big");
    ASSERT_NE(ti, nullptr);
    auto rows = ScanAll(*ti->storage_, ti->schema_);
    ASSERT_EQ(rows.size(), static_cast<size_t>(kRows));
    for (int i = 0; i < kRows; i++) {
      EXPECT_EQ(rows[i].first, i);
      EXPECT_EQ(rows[i].second, "r" + std::to_string(i));
    }
  }
  std::filesystem::remove(path);
}

// A transaction left open at Close() is aborted before the flush, so its uncommitted rows never reach
// disk: reopening sees only the committed baseline. Without the abort-on-close, the uncommitted row —
// stamped with a temp ts whose in-memory undo chain is gone — would be flushed and corrupt the reopen.
TEST(PersistenceTest, OpenTransactionAbortedOnClose) {
  auto path = TempDbPath("bbdb_persist_open_txn.db");
  std::filesystem::remove(path);

  auto one_row = [](const Schema &schema, int32_t id, const std::string &name) {
    DataChunk chunk;
    chunk.Initialize(TypesOf(schema));
    chunk.SetValue(0, 0, Value(id));
    chunk.SetValue(1, 0, Value(name));
    chunk.SetCardinality(1);
    return chunk;
  };

  {
    Database db(path, 32);
    auto ti = db.GetCatalog().CreateTable("t", TwoColSchema());
    auto &tm = db.GetTransactionManager();
    auto &heap = static_cast<TableHeap &>(*ti->storage_);

    // A committed baseline row.
    auto *seed = tm.Begin();
    auto committed = one_row(ti->schema_, 1, "committed");
    Vector seed_rids{LogicalType{LogicalTypeId::BIGINT}};
    MvccInsert(&tm, seed, ti->oid_, heap, committed, seed_rids);
    ASSERT_TRUE(tm.Commit(seed));

    // An uncommitted insert by a txn we deliberately never commit.
    auto *open = tm.Begin();
    auto pending = one_row(ti->schema_, 2, "uncommitted");
    Vector open_rids{LogicalType{LogicalTypeId::BIGINT}};
    MvccInsert(&tm, open, ti->oid_, heap, pending, open_rids);

    db.Close();  // must abort `open`, tombstoning its row before the flush
  }
  {
    Database db(path, 32);
    auto ti = db.GetCatalog().GetTable("t");
    ASSERT_NE(ti, nullptr);
    auto rows = ScanAll(*ti->storage_, ti->schema_);
    ASSERT_EQ(rows.size(), 1U) << "only the committed row survived; the open txn was rolled back";
    EXPECT_EQ(rows[0], std::make_pair(1, std::string("committed")));
  }
  std::filesystem::remove(path);
}

}  // namespace bumblebee
