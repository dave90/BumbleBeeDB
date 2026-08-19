//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// end_to_end_test.cpp
//
// Drives the full SQL path — parse, bind, plan, optimize, lower, execute — through the real
// BumbleBeeInstance. Rows are seeded directly into the heap (INSERT execution lands in a later step).
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bumblebee_instance.h"
#include "catalog/catalog.h"
#include "common/exception.h"
#include "gtest/gtest.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

// SQL-created tables carry an auto BIGINT `_id` primary key at column 0 (see AUTO_ID_COLUMN). These
// helpers seed the heap directly (bypassing the INSERT executor), so they must fill `_id` themselves —
// a sequential value is fine since these tests only read the user columns by name. The two user columns
// therefore live at chunk indices 1 and 2.
const std::vector<LogicalType> kIdTwoInt{LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::INTEGER),
                                         LogicalType(LogicalTypeId::INTEGER)};

/** @brief Append `rows` of (x, y) INT pairs into table `name`'s heap, prefixed with a sequential _id. */
static void SeedTable(BumbleBeeInstance &db, const std::string &name, const std::vector<std::pair<int, int>> &rows) {
  auto info = db.catalog_->GetTable(name);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  ASSERT_NE(heap, nullptr);
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::INTEGER),
                                            LogicalType(LogicalTypeId::INTEGER)});
  for (idx_t i = 0; i < rows.size(); i++) {
    chunk.SetValue(0, i, Value(static_cast<int64_t>(i)));  // _id
    chunk.SetValue(1, i, Value(rows[i].first));
    chunk.SetValue(2, i, Value(rows[i].second));
  }
  chunk.SetCardinality(rows.size());
  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap->Append(chunk, rids);
}

/** @brief Seed `n` rows of (g = i % groups, v = 1) straight into `name`'s heap, spanning many pages. */
static void SeedManyRows(BumbleBeeInstance &db, const std::string &name, int n, int groups) {
  auto info = db.catalog_->GetTable(name);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  ASSERT_NE(heap, nullptr);
  int written = 0;
  while (written < n) {
    idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(kIdTwoInt);
    for (idx_t i = 0; i < batch; i++) {
      int idx = written + static_cast<int>(i);
      chunk.SetValue(0, i, Value(static_cast<int64_t>(idx)));  // _id
      chunk.SetValue(1, i, Value(idx % groups));
      chunk.SetValue(2, i, Value(1));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

/** @brief Seed `n` rows of (x = pseudo-random, id = i) into `name`'s heap. */
static void SeedVaried(BumbleBeeInstance &db, const std::string &name, int n) {
  auto info = db.catalog_->GetTable(name);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  ASSERT_NE(heap, nullptr);
  int written = 0;
  while (written < n) {
    idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(kIdTwoInt);
    for (idx_t i = 0; i < batch; i++) {
      int idx = written + static_cast<int>(i);
      chunk.SetValue(0, i, Value(static_cast<int64_t>(idx)));  // _id
      chunk.SetValue(1, i, Value((idx * 7919) % 100003));      // scattered keys
      chunk.SetValue(2, i, Value(idx));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

static auto RunColumn(BumbleBeeInstance &db, const std::string &sql, int col) -> std::vector<int> {
  StringVectorWriter w;
  db.ExecuteSql(sql, w);
  std::vector<int> out;
  out.reserve(w.values_.size());
  for (const auto &row : w.values_) {
    out.push_back(std::stoi(row[col]));
  }
  return out;
}

/** @brief Seed `n` rows of (k = i % distinct, val = i) into `name`'s heap. */
static void SeedKeyed(BumbleBeeInstance &db, const std::string &name, int n, int distinct) {
  auto info = db.catalog_->GetTable(name);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  ASSERT_NE(heap, nullptr);
  int written = 0;
  while (written < n) {
    idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(kIdTwoInt);
    for (idx_t i = 0; i < batch; i++) {
      int idx = written + static_cast<int>(i);
      chunk.SetValue(0, i, Value(static_cast<int64_t>(idx)));  // _id
      chunk.SetValue(1, i, Value(idx % distinct));
      chunk.SetValue(2, i, Value(idx));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

/**
 * @brief Seed a heavily skewed table: `hot` rows all share key 0 (a single hot key), the rest get
 * distinct keys. Hashing can never split the hot key's partition, so the grace join must fall back to
 * a block nested loop for it — this exercises the NLJ leaf of the recursion tree.
 */
static void SeedHot(BumbleBeeInstance &db, const std::string &name, int n, int hot) {
  auto info = db.catalog_->GetTable(name);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  ASSERT_NE(heap, nullptr);
  int written = 0;
  while (written < n) {
    idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(kIdTwoInt);
    for (idx_t i = 0; i < batch; i++) {
      int idx = written + static_cast<int>(i);
      chunk.SetValue(0, i, Value(static_cast<int64_t>(idx)));  // _id
      chunk.SetValue(1, i, Value(idx < hot ? 0 : idx));        // key 0 is hot; everything past `hot` is unique
      chunk.SetValue(2, i, Value(idx));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

static auto RunHotJoinMultiset(BumbleBeeInstance &db) -> std::multiset<std::pair<int, int>> {
  StringVectorWriter w;
  db.ExecuteSql("CREATE TABLE ha(k INT, v INT);", w);
  db.ExecuteSql("CREATE TABLE hb(k INT, v INT);", w);
  SeedHot(db, "ha", 3000, 2500);  // build: 2500 rows on key 0 (> STANDARD_VECTOR_SIZE budget) -> NLJ leaf
  SeedHot(db, "hb", 550, 50);     // probe: 50 rows on key 0 (keeps the joined result modest)
  db.ExecuteSql("SELECT ha.v, hb.v FROM ha, hb WHERE ha.k = hb.k;", w);
  std::multiset<std::pair<int, int>> rows;
  for (const auto &row : w.values_) {
    rows.emplace(std::stoi(row[0]), std::stoi(row[1]));
  }
  return rows;
}

static auto RunJoinMultiset(BumbleBeeInstance &db) -> std::multiset<std::pair<int, int>> {
  StringVectorWriter w;
  db.ExecuteSql("CREATE TABLE ja(k INT, v INT);", w);
  db.ExecuteSql("CREATE TABLE jb(k INT, v INT);", w);
  SeedKeyed(db, "ja", 3000, 300);
  SeedKeyed(db, "jb", 2000, 300);
  db.ExecuteSql("SELECT ja.v, jb.v FROM ja, jb WHERE ja.k = jb.k;", w);
  std::multiset<std::pair<int, int>> rows;
  for (const auto &row : w.values_) {
    rows.emplace(std::stoi(row[0]), std::stoi(row[1]));
  }
  return rows;
}

/** @brief Run `sql` and return its result rows as a multiset of string cells (so NULLs compare too). */
static auto RunRows(BumbleBeeInstance &db, const std::string &sql) -> std::multiset<std::vector<std::string>> {
  StringVectorWriter w;
  db.ExecuteSql(sql, w);
  std::multiset<std::vector<std::string>> rows;
  for (auto &r : w.values_) {
    rows.insert(r);
  }
  return rows;
}

/** A multi-chunk LEFT join whose right side covers only a third of the left keys (NULL rows matter). */
static auto RunLeftJoinRows(BumbleBeeInstance &db) -> std::multiset<std::vector<std::string>> {
  StringVectorWriter w;
  db.ExecuteSql("CREATE TABLE ja(k INT, v INT);", w);
  db.ExecuteSql("CREATE TABLE jb(k INT, v INT);", w);
  SeedKeyed(db, "ja", 3000, 300);  // keys 0..299
  SeedKeyed(db, "jb", 2000, 100);  // keys 0..99 -> ja keys 100..299 are preserved NULL-padded
  return RunRows(db, "SELECT ja.v, jb.v FROM ja LEFT JOIN jb ON ja.k = jb.k;");
}

/** A LEFT join whose (right) build side is dominated by one hot key — the grace NLJ-fallback path. */
static auto RunHotLeftJoinRows(BumbleBeeInstance &db) -> std::multiset<std::vector<std::string>> {
  StringVectorWriter w;
  db.ExecuteSql("CREATE TABLE ha(k INT, v INT);", w);
  db.ExecuteSql("CREATE TABLE hb(k INT, v INT);", w);
  SeedHot(db, "ha", 550, 50);     // probe/preserved: 50 rows on key 0, keys 50..549 unmatched
  SeedHot(db, "hb", 3000, 2500);  // build: 2500 rows on key 0 (>> resident budget) -> NLJ leaf
  return RunRows(db, "SELECT ha.v, hb.v FROM ha LEFT JOIN hb ON ha.k = hb.k;");
}

// An equi LEFT JOIN lowers to the hash join. Every left row survives: matched rows pair with the right,
// duplicate right keys fan out, and an unmatched left row appears once with the right columns NULL.
TEST(EndToEndTest, LeftJoinEquiPreservesUnmatchedRows) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE a(id INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE b(id INT, y INT);", w));
  db.ExecuteSql("INSERT INTO a VALUES (1,10),(2,20),(3,30);", w);
  db.ExecuteSql("INSERT INTO b VALUES (1,100),(3,300),(3,301);", w);  // id=2 has no match; id=3 has two

  auto got = RunRows(db, "SELECT a.id, a.x, b.y FROM a LEFT JOIN b ON a.id = b.id;");
  std::multiset<std::vector<std::string>> expected{
      {"1", "10", "100"}, {"2", "20", "NULL"}, {"3", "30", "300"}, {"3", "30", "301"}};
  EXPECT_EQ(got, expected);
}

// A LEFT JOIN against an empty right table keeps every left row, all right columns NULL (an INNER join
// would return nothing).
TEST(EndToEndTest, LeftJoinEmptyRightYieldsAllLeftRowsNull) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE a(id INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE b(id INT, y INT);", w));
  db.ExecuteSql("INSERT INTO a VALUES (1,10),(2,20);", w);

  auto got = RunRows(db, "SELECT a.id, b.y FROM a LEFT JOIN b ON a.id = b.id;");
  std::multiset<std::vector<std::string>> expected{{"1", "NULL"}, {"2", "NULL"}};
  EXPECT_EQ(got, expected);
}

// A non-equi LEFT JOIN cannot become a hash join, so it runs on the nested-loop join — which must also
// preserve unmatched left rows NULL-padded.
TEST(EndToEndTest, LeftJoinNonEquiUsesNestedLoop) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE a(id INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE b(id INT, y INT);", w));
  db.ExecuteSql("INSERT INTO a VALUES (1,10),(2,20),(3,30);", w);
  db.ExecuteSql("INSERT INTO b VALUES (1,15),(1,5);", w);  // only a.x=10 is < some b.y (15)

  auto got = RunRows(db, "SELECT a.id, a.x, b.y FROM a LEFT JOIN b ON a.x < b.y;");
  std::multiset<std::vector<std::string>> expected{{"1", "10", "15"}, {"2", "20", "NULL"}, {"3", "30", "NULL"}};
  EXPECT_EQ(got, expected);
}

// A larger equi LEFT JOIN (multi-chunk, exercises the parallel probe + re-entrant emit): every left key
// 0..N-1 must appear, matched keys fanning out by the right multiplicity and unmatched keys NULL-padded.
TEST(EndToEndTest, LeftJoinLargePreservesEveryLeftRow) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE la(k INT, v INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE lb(k INT, v INT);", w));
  SeedKeyed(db, "la", 4000, 4000);  // keys 0..3999, each once
  SeedKeyed(db, "lb", 3000, 1000);  // keys 0..999 (each ~3x); keys 1000..3999 have NO match

  auto got = RunRows(db, "SELECT la.k, lb.v FROM la LEFT JOIN lb ON la.k = lb.k;");
  // Every one of the 4000 left rows appears at least once; the 3000 unmatched left keys are NULL-padded.
  ASSERT_GE(got.size(), 4000u);
  size_t null_rows = 0;
  for (const auto &r : got) {
    if (r[1] == "NULL") {
      null_rows++;
    }
  }
  EXPECT_EQ(null_rows, 3000u) << "left keys 1000..3999 have no right match -> NULL-padded exactly once each";
}

TEST(EndToEndTest, RuntimeSwitchToExternalSortOnOverflow) {
  // prefer_external_ = false, so the FIRST attempt is the in-memory sort. A tiny budget makes it overflow
  // and throw; the driver must re-lower to the external merge sort and retry, returning correct results.
  BumbleBeeInstance db;
  db.max_memory_ = 4096;  // forces the in-memory sort to overflow on the first chunk
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE s(x INT, id INT);", w));
  SeedVaried(db, "s", 8000);

  auto got = RunColumn(db, "SELECT x FROM s ORDER BY x;", 0);
  ASSERT_EQ(got.size(), 8000u);
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));  // correct despite the mid-query switch to disk
}

TEST(EndToEndTest, RuntimeSwitchToGraceJoinOnOverflow) {
  BumbleBeeInstance db;
  db.max_memory_ = 4096;  // forces the in-memory hash-join build to overflow -> retry as grace join
  auto got = RunJoinMultiset(db);
  ASSERT_FALSE(got.empty());

  BumbleBeeInstance oracle;  // in-memory, ample budget
  auto expected = RunJoinMultiset(oracle);
  EXPECT_EQ(got, expected);
}

TEST(EndToEndTest, GraceHashJoinMatchesInMemoryJoin) {
  BumbleBeeInstance mem_db;
  auto expected = RunJoinMultiset(mem_db);
  ASSERT_FALSE(expected.empty());

  BumbleBeeInstance ext_db;
  ext_db.prefer_external_ = true;
  ext_db.max_memory_ = 8192;
  auto got = RunJoinMultiset(ext_db);

  EXPECT_EQ(got, expected);  // same multiset of joined rows
}

TEST(EndToEndTest, GraceHashJoinRecursesOnOversizedPartitions) {
  // 80000 uniform build rows / 64 partitions ~ 1250 rows per partition — above the (floor-1024)
  // resident row budget, so every pair must RE-PARTITION BOTH SIDES with the next 6 hash bits before
  // joining. The recursion allocates thousands of sub-partition spill heaps over the query's
  // lifetime; the run fits the in-memory instance's fixed scratch pages ONLY because dead pairs
  // free their pages eagerly (SpillCollection::Free) — this doubles as the reclamation regression
  // test. The disk-backed run guards the same workload against page-capacity regressions.
  auto run = [](BumbleBeeInstance &db) {
    StringVectorWriter w;
    db.ExecuteSql("CREATE TABLE ra(k INT, v INT);", w);
    db.ExecuteSql("CREATE TABLE rb(k INT, v INT);", w);
    SeedKeyed(db, "ra", 80000, 70000);  // keys 0..69999, ~1.14 rows each
    SeedKeyed(db, "rb", 9000, 70000);   // keys 0..8999
    return RunRows(db, "SELECT ra.v, rb.v FROM ra JOIN rb ON ra.k = rb.k;");
  };
  BumbleBeeInstance mem_db;
  auto expected = run(mem_db);
  ASSERT_FALSE(expected.empty());

  {
    BumbleBeeInstance ext_db;  // in-memory: 4096 scratch pages — only passes with eager reclamation
    ext_db.prefer_external_ = true;
    ext_db.max_memory_ = 8192;  // budget_rows floors at STANDARD_VECTOR_SIZE -> partitions overflow
    auto got = run(ext_db);
    EXPECT_EQ(got, expected);
  }

  const auto db_path = std::filesystem::temp_directory_path() / "bbdb_grace_recursion_test.db";
  std::filesystem::remove(db_path);
  {
    BumbleBeeInstance ext_db(db_path);
    ext_db.prefer_external_ = true;
    ext_db.max_memory_ = 8192;
    auto got = run(ext_db);
    EXPECT_EQ(got, expected);
  }
  std::filesystem::remove(db_path);
}

TEST(EndToEndTest, GraceHashJoinLeftMatchesInMemoryJoin) {
  BumbleBeeInstance mem_db;
  auto expected = RunLeftJoinRows(mem_db);
  ASSERT_FALSE(expected.empty());

  BumbleBeeInstance ext_db;
  ext_db.prefer_external_ = true;
  ext_db.max_memory_ = 8192;
  auto got = RunLeftJoinRows(ext_db);

  EXPECT_EQ(got, expected);  // same multiset, including the NULL-padded preserved rows
}

TEST(EndToEndTest, LeftJoinOverflowSwitchesToGraceJoin) {
  // The in-memory LEFT build now reserves against the budget too: on overflow the driver re-lowers
  // this join to the grace variant, which since the LEFT path landed is a correct target.
  BumbleBeeInstance db;
  db.max_memory_ = 4096;  // forces the in-memory build to overflow -> retry as grace LEFT
  auto got = RunLeftJoinRows(db);
  ASSERT_FALSE(got.empty());

  BumbleBeeInstance oracle;  // in-memory, ample budget
  auto expected = RunLeftJoinRows(oracle);
  EXPECT_EQ(got, expected);
}

TEST(EndToEndTest, GraceLeftJoinHotKeyFallsBackToNestedLoop) {
  // The hot build partition never splits, so the LEFT probe rows for that key go through the
  // block-nested-loop leaf — matched flags must survive that path, and the unmatched preserved rows
  // must still come out exactly once, NULL-padded.
  BumbleBeeInstance mem_db;
  auto expected = RunHotLeftJoinRows(mem_db);
  ASSERT_FALSE(expected.empty());

  BumbleBeeInstance ext_db;
  ext_db.prefer_external_ = true;
  ext_db.max_memory_ = 8192;
  auto got = RunHotLeftJoinRows(ext_db);

  EXPECT_EQ(got, expected);
}

TEST(EndToEndTest, GraceHashJoinFallsBackToNestedLoopOnHotKey) {
  // A single hot key dominates the build, so its partition never splits under recursion — the grace join
  // must block-nested-loop that partition. The joined multiset must still match the in-memory join.
  BumbleBeeInstance mem_db;
  auto expected = RunHotJoinMultiset(mem_db);
  ASSERT_FALSE(expected.empty());

  BumbleBeeInstance ext_db;
  ext_db.prefer_external_ = true;
  ext_db.max_memory_ = 8192;  // hot partition (5000 rows) far exceeds the resident row budget -> NLJ
  auto got = RunHotJoinMultiset(ext_db);

  EXPECT_EQ(got, expected);
}

TEST(EndToEndTest, ExternalMergeSortMatchesInMemorySort) {
  constexpr int kRows = 20000;
  const std::string kQuery = "SELECT x FROM s ORDER BY x;";

  // In-memory sort (the oracle).
  BumbleBeeInstance mem_db;
  StringVectorWriter w;
  ASSERT_TRUE(mem_db.ExecuteSql("CREATE TABLE s(x INT, id INT);", w));
  SeedVaried(mem_db, "s", kRows);
  auto expected = RunColumn(mem_db, kQuery, 0);
  ASSERT_EQ(expected.size(), static_cast<size_t>(kRows));
  ASSERT_TRUE(std::is_sorted(expected.begin(), expected.end()));

  // External merge sort with a tiny budget -> many runs -> a real multi-run merge.
  BumbleBeeInstance ext_db;
  ext_db.prefer_external_ = true;
  ext_db.max_memory_ = 8192;
  ASSERT_TRUE(ext_db.ExecuteSql("CREATE TABLE s(x INT, id INT);", w));
  SeedVaried(ext_db, "s", kRows);
  auto got = RunColumn(ext_db, kQuery, 0);

  EXPECT_EQ(got, expected);  // identical rows, identical order
}

TEST(EndToEndTest, ParallelScanAggregateOverManyMorsels) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE big(g INT, v INT);", w));
  constexpr int kRows = 60000;  // hundreds of pages -> multiple scan morsels -> multiple parallel tasks
  constexpr int kGroups = 10;
  SeedManyRows(db, "big", kRows, kGroups);

  // COUNT(*) fans the scan across tasks; the ungrouped aggregate's Combine merges their partials.
  ASSERT_TRUE(db.ExecuteSql("SELECT COUNT(*) FROM big;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), kRows);

  // GROUP BY exercises the concurrent hash-aggregate Sink + locked Combine across tasks.
  ASSERT_TRUE(db.ExecuteSql("SELECT g, SUM(v) FROM big GROUP BY g;", w));
  ASSERT_EQ(w.values_.size(), static_cast<size_t>(kGroups));
  int total = 0;
  for (const auto &row : w.values_) {
    total += std::stoi(row[1]);
  }
  EXPECT_EQ(total, kRows);  // every row counted exactly once across all groups
}

TEST(EndToEndTest, SelectStarReturnsEveryRow) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  SeedTable(db, "t", {{1, 10}, {2, 20}, {3, 30}, {4, 40}, {5, 50}});

  ASSERT_TRUE(db.ExecuteSql("SELECT * FROM t;", w));
  ASSERT_EQ(w.values_.size(), 5u);

  std::vector<int> xs;
  for (const auto &row : w.values_) {
    ASSERT_EQ(row.size(), 3u);        // _id, x, y  (SELECT * now includes the auto primary key)
    xs.push_back(std::stoi(row[1]));  // x is column 1; column 0 is _id
  }
  std::sort(xs.begin(), xs.end());
  EXPECT_EQ(xs, (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST(EndToEndTest, SelectWithFilterAndProjection) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  SeedTable(db, "t", {{1, 10}, {2, 20}, {3, 30}, {4, 40}, {5, 50}});

  ASSERT_TRUE(db.ExecuteSql("SELECT y FROM t WHERE x > 2;", w));

  std::vector<int> ys;
  for (const auto &row : w.values_) {
    ASSERT_EQ(row.size(), 1u);
    ys.push_back(std::stoi(row[0]));
  }
  std::sort(ys.begin(), ys.end());
  EXPECT_EQ(ys, (std::vector<int>{30, 40, 50}));  // x in {3,4,5}
}

TEST(EndToEndTest, InsertValuesThenSelectRoundTrips) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));

  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);", w));
  ASSERT_EQ(w.values_.size(), 1u);           // one row: the affected-row count
  EXPECT_EQ(std::stoi(w.values_[0][0]), 3);  // three rows inserted

  ASSERT_TRUE(db.ExecuteSql("SELECT x, y FROM t;", w));
  ASSERT_EQ(w.values_.size(), 3u);
  std::vector<int> xs;
  for (const auto &row : w.values_) {
    xs.push_back(std::stoi(row[0]));
  }
  std::sort(xs.begin(), xs.end());
  EXPECT_EQ(xs, (std::vector<int>{1, 2, 3}));

  ASSERT_TRUE(db.ExecuteSql("SELECT y FROM t WHERE x = 2;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), 20);
}

TEST(EndToEndTest, DeleteWithPredicateRemovesMatchingRows) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40);", w));

  ASSERT_TRUE(db.ExecuteSql("DELETE FROM t WHERE x > 2;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), 2);  // rows x=3, x=4 deleted

  ASSERT_TRUE(db.ExecuteSql("SELECT x FROM t;", w));
  std::vector<int> xs;
  for (const auto &row : w.values_) {
    xs.push_back(std::stoi(row[0]));
  }
  std::sort(xs.begin(), xs.end());
  EXPECT_EQ(xs, (std::vector<int>{1, 2}));
}

TEST(EndToEndTest, UpdateRecomputesTargetedColumns) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);", w));

  ASSERT_TRUE(db.ExecuteSql("UPDATE t SET y = y + 100 WHERE x = 2;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), 1);  // one row updated

  ASSERT_TRUE(db.ExecuteSql("SELECT y FROM t WHERE x = 2;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), 120);
}

TEST(EndToEndTest, OrderByAscendingAndDescending) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (3, 1), (1, 2), (2, 3), (5, 4), (4, 5);", w));

  ASSERT_TRUE(db.ExecuteSql("SELECT x FROM t ORDER BY x;", w));
  std::vector<int> asc;
  for (const auto &row : w.values_) {
    asc.push_back(std::stoi(row[0]));
  }
  EXPECT_EQ(asc, (std::vector<int>{1, 2, 3, 4, 5}));

  ASSERT_TRUE(db.ExecuteSql("SELECT x FROM t ORDER BY x DESC;", w));
  std::vector<int> desc;
  for (const auto &row : w.values_) {
    desc.push_back(std::stoi(row[0]));
  }
  EXPECT_EQ(desc, (std::vector<int>{5, 4, 3, 2, 1}));
}

TEST(EndToEndTest, OrderByLimitCollapsesToTopN) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (3, 1), (1, 2), (2, 3), (5, 4), (4, 5);", w));

  ASSERT_TRUE(db.ExecuteSql("SELECT x FROM t ORDER BY x DESC LIMIT 2;", w));
  std::vector<int> top;
  for (const auto &row : w.values_) {
    top.push_back(std::stoi(row[0]));
  }
  EXPECT_EQ(top, (std::vector<int>{5, 4}));
}

TEST(EndToEndTest, BareLimitTruncates) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (1, 1), (2, 2), (3, 3), (4, 4);", w));

  ASSERT_TRUE(db.ExecuteSql("SELECT x FROM t LIMIT 2;", w));
  EXPECT_EQ(w.values_.size(), 2u);
}

TEST(EndToEndTest, UngroupedAggregates) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);", w));

  ASSERT_TRUE(db.ExecuteSql("SELECT COUNT(*), SUM(y) FROM t;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), 3);   // count
  EXPECT_EQ(std::stoi(w.values_[0][1]), 60);  // sum(y)
}

TEST(EndToEndTest, CountStarOnEmptyTableIsZero) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("SELECT COUNT(*) FROM t;", w));
  ASSERT_EQ(w.values_.size(), 1u);
  EXPECT_EQ(std::stoi(w.values_[0][0]), 0);
}

TEST(EndToEndTest, GroupByWithSum) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(g INT, v INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO t VALUES (1, 10), (1, 20), (2, 5), (2, 7), (3, 100);", w));

  ASSERT_TRUE(db.ExecuteSql("SELECT g, SUM(v) FROM t GROUP BY g;", w));
  ASSERT_EQ(w.values_.size(), 3u);
  std::map<int, int> sums;
  for (const auto &row : w.values_) {
    sums[std::stoi(row[0])] = std::stoi(row[1]);
  }
  EXPECT_EQ(sums[1], 30);
  EXPECT_EQ(sums[2], 12);
  EXPECT_EQ(sums[3], 100);
}

TEST(EndToEndTest, HashJoinOnEquiKey) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE ta(k INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE tb(k INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO ta VALUES (1, 100), (2, 200), (3, 300);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO tb VALUES (2, 20), (3, 30), (4, 40);", w));

  // Implicit join + equi-predicate -> optimizer produces a hash join.
  ASSERT_TRUE(db.ExecuteSql("SELECT ta.x, tb.y FROM ta, tb WHERE ta.k = tb.k;", w));
  ASSERT_EQ(w.values_.size(), 2u);  // k in {2, 3}
  std::map<int, int> joined;
  for (const auto &row : w.values_) {
    joined[std::stoi(row[0])] = std::stoi(row[1]);
  }
  EXPECT_EQ(joined[200], 20);  // ta.x=200 (k=2) with tb.y=20
  EXPECT_EQ(joined[300], 30);  // ta.x=300 (k=3) with tb.y=30
}

// A WHERE filter over an explicit `INNER JOIN ... ON` is folded into the join and pushed to the
// probe-side scan by the optimizer. This checks that rewrite is result-preserving: only rows whose
// single-table predicate holds survive, and the ON key still governs the pairing.
TEST(EndToEndTest, InnerJoinOnWithWhereFilterPushesDownAndStaysCorrect) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE ta(k INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE tb(k INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO ta VALUES (1, 5), (2, 20), (3, 30);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO tb VALUES (1, 11), (2, 22), (3, 33);", w));

  // ta.k=tb.k pairs all three keys; ta.x>10 then drops the (k=1) pair whose ta.x is 5.
  ASSERT_TRUE(db.ExecuteSql("SELECT ta.x, tb.y FROM ta JOIN tb ON ta.k = tb.k WHERE ta.x > 10;", w));
  ASSERT_EQ(w.values_.size(), 2u);
  std::map<int, int> joined;
  for (const auto &row : w.values_) {
    joined[std::stoi(row[0])] = std::stoi(row[1]);
  }
  EXPECT_EQ(joined.count(5), 0u);  // filtered out
  EXPECT_EQ(joined[20], 22);       // ta.x=20 (k=2) with tb.y=22
  EXPECT_EQ(joined[30], 33);       // ta.x=30 (k=3) with tb.y=33

  // The explicit-ON form must match the comma-join form the optimizer already handled.
  StringVectorWriter comma;
  ASSERT_TRUE(db.ExecuteSql("SELECT ta.x, tb.y FROM ta, tb WHERE ta.k = tb.k AND ta.x > 10;", comma));
  EXPECT_EQ(comma.values_.size(), 2u);
}

TEST(EndToEndTest, HashJoinEmptyBuildYieldsNoRows) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE ta(k INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE tb(k INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO tb VALUES (2, 20), (3, 30);", w));  // ta is empty

  ASSERT_TRUE(db.ExecuteSql("SELECT ta.x, tb.y FROM ta, tb WHERE ta.k = tb.k;", w));
  EXPECT_EQ(w.values_.size(), 0u);  // empty build -> NO_OUTPUT_POSSIBLE, tb never scanned
}

TEST(EndToEndTest, NestedLoopJoinNonEquiPredicate) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE ta(k INT, x INT);", w));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE tb(k INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO ta VALUES (1, 100), (2, 200);", w));
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO tb VALUES (2, 20), (3, 30);", w));

  // A non-equi predicate stays a nested-loop join.
  ASSERT_TRUE(db.ExecuteSql("SELECT ta.x, tb.y FROM ta, tb WHERE ta.k < tb.k;", w));
  ASSERT_EQ(w.values_.size(), 3u);  // (1<2),(1<3),(2<3)
}

TEST(EndToEndTest, SelectFromEmptyTableReturnsNoRows) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(x INT, y INT);", w));
  ASSERT_TRUE(db.ExecuteSql("SELECT * FROM t;", w));
  EXPECT_EQ(w.values_.size(), 0u);
}

// A table declared without a PRIMARY KEY gets an auto BIGINT `_id` column (visible, at position 0),
// auto-filled with a per-table counter, and a B+tree index is built on it.
TEST(EndToEndTest, AutoIdPrimaryKeyGeneratedAndIndexed) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(a INT);", w));
  db.ExecuteSql("INSERT INTO t VALUES (10),(20),(30);", w);

  auto got = RunRows(db, "SELECT * FROM t;");  // _id, a
  std::multiset<std::vector<std::string>> expected{{"0", "10"}, {"1", "20"}, {"2", "30"}};
  EXPECT_EQ(got, expected);
  EXPECT_NE(db.catalog_->GetIndex("_pk_t", "t"), NULL_INDEX_INFO) << "the primary-key index must exist";
}

// `_id` is a normal identifier, so it is usable unquoted in queries: projected, filtered, and ordered by.
TEST(EndToEndTest, AutoIdIsUsableInQueries) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(a INT);", w));
  db.ExecuteSql("INSERT INTO t VALUES (10),(20),(30);", w);  // _id 0,1,2

  const std::string q = "SELECT _id, a FROM t WHERE _id >= 1 ORDER BY _id DESC;";
  EXPECT_EQ(RunColumn(db, q, 0), (std::vector<int>{2, 1}));    // _id, filtered and ordered
  EXPECT_EQ(RunColumn(db, q, 1), (std::vector<int>{30, 20}));  // the matching a values
}

// A declared PRIMARY KEY adds no `_id`, indexes the declared column, and rejects a duplicate key.
TEST(EndToEndTest, ExplicitPrimaryKeyEnforcesUniqueness) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE u(id INT PRIMARY KEY, v INT);", w));
  db.ExecuteSql("INSERT INTO u VALUES (1,10),(2,20);", w);

  EXPECT_THROW(db.ExecuteSql("INSERT INTO u VALUES (1,99);", w), Exception);  // duplicate primary key

  auto got = RunRows(db, "SELECT id, v FROM u;");
  std::multiset<std::vector<std::string>> expected{{"1", "10"}, {"2", "20"}};  // the dup left no trace
  EXPECT_EQ(got, expected);
  EXPECT_NE(db.catalog_->GetIndex("_pk_u", "u"), NULL_INDEX_INFO);
}

// Index maintenance is transactional: a failed multi-row INSERT rolls its index entries back, so the
// first row's key is not leaked and can be inserted cleanly afterwards.
TEST(EndToEndTest, FailedInsertRollsBackIndexKey) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE p(id INT PRIMARY KEY, v INT);", w));
  db.ExecuteSql("INSERT INTO p VALUES (1,10);", w);

  // (2,20) is inserted into the index, then (2,21) trips the duplicate and aborts the statement.
  EXPECT_THROW(db.ExecuteSql("INSERT INTO p VALUES (2,20),(2,21);", w), Exception);

  // Key 2 was rolled back out of the index, so this succeeds (it would fail if 2 had leaked).
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO p VALUES (2,99);", w));
  auto got = RunRows(db, "SELECT id, v FROM p;");
  std::multiset<std::vector<std::string>> expected{{"1", "10"}, {"2", "99"}};
  EXPECT_EQ(got, expected);
}

// The primary-key index is a stable key -> RID directory: deleting a key leaves its entry in place, and
// re-inserting the key REVIVES that slot with the new value. Repeated delete+reinsert cycles stay correct
// and keep enforcing uniqueness.
TEST(EndToEndTest, DeletedKeyIsRevivedWithNewValue) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE p(id INT PRIMARY KEY, v INT);", w));
  db.ExecuteSql("INSERT INTO p VALUES (1,10),(2,20),(3,30);", w);

  for (int cycle = 1; cycle <= 3; cycle++) {
    ASSERT_TRUE(db.ExecuteSql("DELETE FROM p;", w));
    const int base = cycle * 100;
    db.ExecuteSql("INSERT INTO p VALUES (1," + std::to_string(base + 1) + "),(2," + std::to_string(base + 2) + "),(3," +
                      std::to_string(base + 3) + ");",
                  w);
    auto got = RunRows(db, "SELECT id, v FROM p;");
    std::multiset<std::vector<std::string>> expected{
        {"1", std::to_string(base + 1)}, {"2", std::to_string(base + 2)}, {"3", std::to_string(base + 3)}};
    EXPECT_EQ(got, expected) << "cycle " << cycle;
    // The revived keys are live again, so uniqueness is enforced.
    EXPECT_THROW(db.ExecuteSql("INSERT INTO p VALUES (2,-1);", w), Exception);
  }
}

// Explicit ROLLBACK undoes INSERT / DELETE / UPDATE index changes, keeping the index consistent with the
// rolled-back heap: freed keys become insertable, restored keys stay unique.
TEST(EndToEndTest, RollbackRestoresIndexForAllVerbs) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE p(id INT PRIMARY KEY, v INT);", w));
  db.ExecuteSql("INSERT INTO p VALUES (1,10),(2,20);", w);

  // INSERT then ROLLBACK: the new keys are freed.
  db.ExecuteSql("BEGIN;", w);
  db.ExecuteSql("INSERT INTO p VALUES (3,30);", w);
  db.ExecuteSql("ROLLBACK;", w);
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO p VALUES (3,300);", w));  // key 3 was released

  // DELETE then ROLLBACK: the deleted key is restored (so re-inserting it is a duplicate).
  db.ExecuteSql("BEGIN;", w);
  db.ExecuteSql("DELETE FROM p WHERE id = 1;", w);
  db.ExecuteSql("ROLLBACK;", w);
  EXPECT_THROW(db.ExecuteSql("INSERT INTO p VALUES (1,-1);", w), Exception);

  // UPDATE key move then ROLLBACK: the new key is freed, the old key stays occupied.
  db.ExecuteSql("BEGIN;", w);
  db.ExecuteSql("UPDATE p SET id = 50 WHERE id = 2;", w);
  db.ExecuteSql("ROLLBACK;", w);
  ASSERT_TRUE(db.ExecuteSql("INSERT INTO p VALUES (50,5);", w));              // new key 50 was never committed
  EXPECT_THROW(db.ExecuteSql("INSERT INTO p VALUES (2,-2);", w), Exception);  // old key 2 restored

  auto got = RunRows(db, "SELECT id, v FROM p;");
  std::multiset<std::vector<std::string>> expected{{"1", "10"}, {"2", "20"}, {"3", "300"}, {"50", "5"}};
  EXPECT_EQ(got, expected);
}

// `_id` is reserved: a user cannot declare a column of that name.
TEST(EndToEndTest, ReservedIdColumnNameIsRejected) {
  BumbleBeeInstance db;
  StringVectorWriter w;
  EXPECT_THROW(db.ExecuteSql("CREATE TABLE bad(\"_id\" INT, x INT);", w), Exception);
}

// The `_id` auto-increment high-water mark is persisted: after reopening the database, new rows keep
// getting fresh ids rather than colliding with existing ones.
TEST(EndToEndTest, AutoIdCounterSurvivesRestart) {
  const auto path = std::filesystem::temp_directory_path() / "bbdb_autoid_restart.db";
  std::filesystem::remove(path);
  {
    BumbleBeeInstance db(path);
    StringVectorWriter w;
    ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(a INT);", w));
    db.ExecuteSql("INSERT INTO t VALUES (10),(20);", w);  // ids 0,1
  }
  {
    BumbleBeeInstance db(path);
    StringVectorWriter w;
    db.ExecuteSql("INSERT INTO t VALUES (30),(40);", w);  // ids must continue at 2,3
    auto got = RunRows(db, "SELECT * FROM t;");
    std::multiset<std::vector<std::string>> expected{{"0", "10"}, {"1", "20"}, {"2", "30"}, {"3", "40"}};
    EXPECT_EQ(got, expected);
  }
  std::filesystem::remove(path);
}

}  // namespace bumblebee
