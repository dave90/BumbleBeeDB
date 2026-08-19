//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parallel_execution_test.cpp
//
// Identification: test/unit/execution/parallel_execution_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// Multi-threaded coverage for the REAL physical operators whose parallel Sink merges local worker
// state into shared global state under a lock: hash join, hash aggregate, ungrouped aggregate, and
// sort. Each query is forced to run with several worker threads and one heap page per morsel (so the
// parallel scan hands every worker its own slice of the build), then repeated so a racy merge shows up
// as a wrong result on some iteration. Correctness is checked against a serial reference computed in
// the test from the same row generators.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
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

// SQL-created tables carry an auto BIGINT `_id` at column 0, so a (k, v) table is [_id, k, v]. We seed
// the heap directly (bypassing the INSERT executor) and fill `_id` with a sequential value.
const std::vector<LogicalType> kIdTwoInt{LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::INTEGER),
                                         LogicalType(LogicalTypeId::INTEGER)};

/** @brief Append `n` rows of (k = fk(i), v = fv(i)) into `name`'s heap, spanning many pages. */
static void SeedHeap(BumbleBeeInstance &db, const std::string &name, int n, const std::function<int(int)> &fk,
                     const std::function<int(int)> &fv) {
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
      chunk.SetValue(1, i, Value(fk(idx)));
      chunk.SetValue(2, i, Value(fv(idx)));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

/** @brief Force intra-query parallelism: several workers and one heap page per parallel-scan morsel. */
static void ConfigureParallel(BumbleBeeInstance &db) {
  db.max_threads_ = 8;   // clamped to [1, MAX_THREADS]; spawns this many query workers
  db.morsel_pages_ = 1;  // one page per morsel => many morsels => real parallel build
}

/** @brief Run `sql` and collect the result as `num_cols`-wide integer rows. */
static auto RunIntRows(BumbleBeeInstance &db, const std::string &sql, int num_cols) -> std::vector<std::vector<int>> {
  StringVectorWriter w;
  db.ExecuteSql(sql, w);
  std::vector<std::vector<int>> out;
  out.reserve(w.values_.size());
  for (const auto &row : w.values_) {
    std::vector<int> parsed;
    parsed.reserve(num_cols);
    for (int c = 0; c < num_cols; c++) {
      parsed.push_back(std::stoi(row[c]));
    }
    out.push_back(std::move(parsed));
  }
  return out;
}

/** @brief Append `n` rows of (g = fg(i), s = fs(i)) into `name`'s heap, spanning many pages.
 *  The chunk types come from the table's own schema, so the VARCHAR column matches exactly. */
static void SeedStringHeap(BumbleBeeInstance &db, const std::string &name, int n, const std::function<int(int)> &fg,
                           const std::function<std::string(int)> &fs) {
  auto info = db.catalog_->GetTable(name);
  auto *heap = dynamic_cast<TableHeap *>(info->storage_.get());
  ASSERT_NE(heap, nullptr);
  std::vector<LogicalType> types;  // [_id BIGINT, g INT, s VARCHAR]
  for (uint32_t c = 0; c < info->schema_.GetColumnCount(); c++) {
    types.push_back(info->schema_.GetColumn(c).GetType());
  }
  int written = 0;
  while (written < n) {
    idx_t batch = std::min<idx_t>(STANDARD_VECTOR_SIZE, n - written);
    DataChunk chunk;
    chunk.Initialize(types);
    for (idx_t i = 0; i < batch; i++) {
      int idx = written + static_cast<int>(i);
      chunk.SetValue(0, i, Value(static_cast<int64_t>(idx)));  // _id
      chunk.SetValue(1, i, Value(fg(idx)));
      chunk.SetValue(2, i, Value(fs(idx)));
    }
    chunk.SetCardinality(batch);
    Vector rids{LogicalType{LogicalTypeId::BIGINT}};
    heap->Append(chunk, rids);
    written += static_cast<int>(batch);
  }
}

/** @brief Run `sql` and collect `num_cols`-wide string rows, stripping the printer's surrounding quotes. */
static auto RunStringRows(BumbleBeeInstance &db, const std::string &sql, int num_cols)
    -> std::vector<std::vector<std::string>> {
  StringVectorWriter w;
  db.ExecuteSql(sql, w);
  std::vector<std::vector<std::string>> out;
  out.reserve(w.values_.size());
  for (const auto &row : w.values_) {
    std::vector<std::string> parsed;
    parsed.reserve(num_cols);
    for (int c = 0; c < num_cols; c++) {
      std::string cell = row[c];
      if (cell.size() >= 2 && cell.front() == '\'' && cell.back() == '\'') {
        cell = cell.substr(1, cell.size() - 2);
      }
      parsed.push_back(std::move(cell));
    }
    out.push_back(std::move(parsed));
  }
  return out;
}

// Parallel hash join: build + probe run across workers, matches merge into the shared hash table /
// output. The multiset of (a.v, b.v) pairs must equal the serial join on every repeat.
TEST(ParallelExecutionTest, ParallelHashJoinMatchesSerial) {
  const int kA = 2048;
  const int kB = 2048;
  const int kDistinct = 256;
  auto key_a = [&](int i) { return i % kDistinct; };
  auto val_a = [&](int i) { return i; };
  auto key_b = [&](int i) { return i % kDistinct; };
  auto val_b = [&](int i) { return i + 1'000'000; };

  // Serial reference: group each side's values by key, then form every (av, bv) pair per key.
  std::map<int, std::vector<int>> a_by_key;
  std::map<int, std::vector<int>> b_by_key;
  for (int i = 0; i < kA; i++) {
    a_by_key[key_a(i)].push_back(val_a(i));
  }
  for (int i = 0; i < kB; i++) {
    b_by_key[key_b(i)].push_back(val_b(i));
  }
  std::multiset<std::pair<int, int>> expected;
  for (const auto &[k, avs] : a_by_key) {
    auto it = b_by_key.find(k);
    if (it == b_by_key.end()) {
      continue;
    }
    for (int av : avs) {
      for (int bv : it->second) {
        expected.emplace(av, bv);
      }
    }
  }

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE a(k INT, v INT);", noop));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE b(k INT, v INT);", noop));
  SeedHeap(db, "a", kA, key_a, val_a);
  SeedHeap(db, "b", kB, key_b, val_b);

  for (int rep = 0; rep < 6; rep++) {
    auto rows = RunIntRows(db, "SELECT a.v, b.v FROM a JOIN b ON a.k = b.k;", 2);
    std::multiset<std::pair<int, int>> got;
    for (const auto &r : rows) {
      got.emplace(r[0], r[1]);
    }
    ASSERT_EQ(got, expected) << "mismatch on repeat " << rep;
  }
}

// Parallel SEMI/ANTI hash join (IN / NOT IN subquery): the subquery side builds across workers, the
// outer side probes across workers, and each outer row must appear exactly once (SEMI, key present)
// or exactly once (ANTI, key absent) — duplicates in the build must never duplicate probe rows.
TEST(ParallelExecutionTest, ParallelSemiAntiJoinMatchesSerial) {
  const int kA = 2048;
  const int kB = 2048;
  auto key_a = [&](int i) { return i % 256; };  // outer keys 0..255
  auto val_a = [&](int i) { return i; };
  auto key_b = [&](int i) { return i % 128; };  // subquery holds only 0..127, each many times over
  auto val_b = [&](int i) { return i; };

  std::multiset<int> expected_semi;
  std::multiset<int> expected_anti;
  for (int i = 0; i < kA; i++) {
    (key_a(i) < 128 ? expected_semi : expected_anti).insert(val_a(i));
  }

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE a(k INT, v INT);", noop));
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE b(k INT, v INT);", noop));
  SeedHeap(db, "a", kA, key_a, val_a);
  SeedHeap(db, "b", kB, key_b, val_b);

  for (int rep = 0; rep < 6; rep++) {
    auto semi_rows = RunIntRows(db, "SELECT a.v FROM a WHERE a.k IN (SELECT b.k FROM b);", 1);
    std::multiset<int> semi;
    for (const auto &r : semi_rows) {
      semi.insert(r[0]);
    }
    ASSERT_EQ(semi, expected_semi) << "semi mismatch on repeat " << rep;

    auto anti_rows = RunIntRows(db, "SELECT a.v FROM a WHERE a.k NOT IN (SELECT b.k FROM b);", 1);
    std::multiset<int> anti;
    for (const auto &r : anti_rows) {
      anti.insert(r[0]);
    }
    ASSERT_EQ(anti, expected_anti) << "anti mismatch on repeat " << rep;
  }
}

// Parallel hash aggregate: each worker builds partial groups that merge into the global table. Per-group
// COUNT/SUM must match the serial aggregation on every repeat.
TEST(ParallelExecutionTest, ParallelHashAggregateMatchesSerial) {
  const int kRows = 6000;
  const int kGroups = 128;
  auto grp = [&](int i) { return i % kGroups; };
  auto val = [&](int i) { return (i % 7) + 1; };

  std::map<int, std::pair<int, long>> expected;  // g -> (count, sum)
  for (int i = 0; i < kRows; i++) {
    auto &e = expected[grp(i)];
    e.first += 1;
    e.second += val(i);
  }

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(g INT, v INT);", noop));
  SeedHeap(db, "t", kRows, grp, val);

  for (int rep = 0; rep < 6; rep++) {
    auto rows = RunIntRows(db, "SELECT g, COUNT(*), SUM(v) FROM t GROUP BY g;", 3);
    std::map<int, std::pair<int, long>> got;
    for (const auto &r : rows) {
      got[r[0]] = {r[1], r[2]};
    }
    ASSERT_EQ(got, expected) << "mismatch on repeat " << rep;
  }
}

// Parallel grouped MIN/MAX over VARCHAR: each worker keeps its group extremes in its own off-row
// string heap, and Combine ships them across the merge boundary as real strings. Per-group MIN/MAX
// must match the serial lexicographic aggregation on every repeat (TSan proves the per-task heaps
// and str_vals_ are never shared).
TEST(ParallelExecutionTest, ParallelGroupedStringMinMaxMatchesSerial) {
  const int kRows = 6000;
  const int kGroups = 128;
  auto grp = [&](int i) { return i % kGroups; };
  // Zero-padded so lexicographic order == numeric order; scattered so extremes land on varied rows.
  auto sval = [&](int i) {
    unsigned v = static_cast<unsigned>(i) * 2654435761u % 100000u;
    std::string s = std::to_string(v);
    return std::string(5 - s.size(), '0') + s;
  };

  std::map<int, std::pair<std::string, std::string>> expected;  // g -> (min, max)
  for (int i = 0; i < kRows; i++) {
    std::string s = sval(i);
    auto it = expected.find(grp(i));
    if (it == expected.end()) {
      expected[grp(i)] = {s, s};
    } else {
      it->second.first = std::min(it->second.first, s);
      it->second.second = std::max(it->second.second, s);
    }
  }

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(g INT, s VARCHAR);", noop));
  SeedStringHeap(db, "t", kRows, grp, sval);

  for (int rep = 0; rep < 6; rep++) {
    auto rows = RunStringRows(db, "SELECT g, MIN(s), MAX(s) FROM t GROUP BY g;", 3);
    std::map<int, std::pair<std::string, std::string>> got;
    for (const auto &r : rows) {
      got[std::stoi(r[0])] = {r[1], r[2]};
    }
    ASSERT_EQ(got, expected) << "mismatch on repeat " << rep;
  }
}

// Parallel ungrouped aggregate: workers combine partial COUNT/SUM into one global scalar.
TEST(ParallelExecutionTest, ParallelUngroupedAggregateMatchesSerial) {
  const int kRows = 6000;
  auto val = [&](int i) { return (i % 11) + 1; };

  long expected_sum = 0;
  for (int i = 0; i < kRows; i++) {
    expected_sum += val(i);
  }

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(k INT, v INT);", noop));
  SeedHeap(db, "t", kRows, [](int i) { return i; }, val);

  for (int rep = 0; rep < 6; rep++) {
    auto rows = RunIntRows(db, "SELECT COUNT(*), SUM(v) FROM t;", 2);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0], kRows) << "count mismatch on repeat " << rep;
    EXPECT_EQ(rows[0][1], expected_sum) << "sum mismatch on repeat " << rep;
  }
}

// High-cardinality aggregate: the lowered partitioning threshold pushes every sink task onto the
// adaptive-partitioning path, so this exercises the repartition scan, the partitioned insert
// path, the whole-table Combine hand-off, and the source's adopt-and-merge — none of which the
// small aggregates above ever reach. Totals must still be exact on every repeat.
TEST(ParallelExecutionTest, ParallelHighCardinalityAggregateMatchesSerial) {
  const int kRows = 600000;  // every row its own group; ~75k groups per worker

  BumbleBeeInstance db;
  ConfigureParallel(db);
  db.agg_partition_threshold_ = 10000;  // far below each task's group count: all tasks partition
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(k INT, v INT);", noop));
  // Every row its own group; v = k % 97 gives a checkable SUM.
  SeedHeap(db, "t", kRows, [](int i) { return i; }, [](int i) { return i % 97; });

  long expected_sum = 0;
  for (int i = 0; i < kRows; i++) {
    expected_sum += i % 97;
  }

  for (int rep = 0; rep < 3; rep++) {
    // The outer aggregate folds the huge GROUP BY's output, so a lost or double-counted group
    // shows up in all three columns.
    auto rows = RunIntRows(db,
                           "SELECT COUNT(*), SUM(c), SUM(s) FROM "
                           "(SELECT k, COUNT(*) AS c, SUM(v) AS s FROM t GROUP BY k);",
                           3);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0][0], kRows) << "group count mismatch on repeat " << rep;
    EXPECT_EQ(rows[0][1], kRows) << "per-group counts mismatch on repeat " << rep;
    EXPECT_EQ(rows[0][2], expected_sum) << "per-group sums mismatch on repeat " << rep;
  }
}

// String MIN/MAX across the adaptive partitioning boundary: the lowered threshold forces the
// partitioned path, so the string extremes must survive the repartition scan (index -> transport
// VARCHAR -> re-fold) and the source's adopt-and-merge.
TEST(ParallelExecutionTest, HighCardinalityStringMinMaxSurvivesRepartition) {
  const int kRows = 300000;
  auto grp = [](int i) { return i; };  // every row its own group
  auto sval = [](int i) {
    unsigned v = static_cast<unsigned>(i) * 2654435761u % 100000u;
    std::string s = std::to_string(v);
    return std::string(5 - s.size(), '0') + s;
  };

  BumbleBeeInstance db;
  ConfigureParallel(db);
  db.agg_partition_threshold_ = 10000;  // force the partitioned path
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(g INT, s VARCHAR);", noop));
  SeedStringHeap(db, "t", kRows, grp, sval);

  // Every group has exactly one row, so MIN(s) == the row's value; fold the check into the group
  // count plus the numeric extremes of the recovered strings.
  auto rows = RunIntRows(
      db, "SELECT COUNT(*), MIN(CAST(m AS INT)), MAX(CAST(m AS INT)) FROM (SELECT g, MIN(s) AS m FROM t GROUP BY g);",
      3);
  int expected_min = std::numeric_limits<int>::max();
  int expected_max = 0;
  for (int i = 0; i < kRows; i++) {
    const int v = static_cast<int>(static_cast<unsigned>(i) * 2654435761u % 100000u);
    expected_min = std::min(expected_min, v);
    expected_max = std::max(expected_max, v);
  }
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0][0], kRows);
  EXPECT_EQ(rows[0][1], expected_min);
  EXPECT_EQ(rows[0][2], expected_max);
}

// Parallel result collection: the final pipeline (scan -> collector) now runs with many workers, each
// tagging its chunks with the source morsel's batch index; the collector's Finalize sorts by it. The
// row order of a bare SELECT must therefore equal the serial scan (= insertion) order on every repeat —
// a racy or unsorted collector shows up as a permuted result.
TEST(ParallelExecutionTest, ParallelScanCollectorPreservesSerialOrder) {
  const int kRows = 6000;

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(k INT, v INT);", noop));
  SeedHeap(db, "t", kRows, [](int i) { return i; }, [](int i) { return i * 3; });

  for (int rep = 0; rep < 6; rep++) {
    auto rows = RunIntRows(db, "SELECT k, v FROM t;", 2);
    ASSERT_EQ(rows.size(), static_cast<size_t>(kRows)) << "wrong row count on repeat " << rep;
    for (int i = 0; i < kRows; i++) {
      ASSERT_EQ(rows[i][0], i) << "row out of serial scan order on repeat " << rep;
      ASSERT_EQ(rows[i][1], i * 3) << "row out of serial scan order on repeat " << rep;
    }
  }
}

// Parallel sort: workers sort local runs that merge into one ordered output. The result must be fully
// ordered and contain exactly the seeded keys on every repeat.
TEST(ParallelExecutionTest, ParallelSortIsFullyOrdered) {
  const int kRows = 6000;
  auto key = [&](int i) { return (i * 7919) % 100003; };  // scattered

  std::vector<int> expected;
  expected.reserve(kRows);
  for (int i = 0; i < kRows; i++) {
    expected.push_back(key(i));
  }
  std::sort(expected.begin(), expected.end());

  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE t(k INT, v INT);", noop));
  SeedHeap(db, "t", kRows, key, [](int i) { return i; });

  for (int rep = 0; rep < 6; rep++) {
    auto rows = RunIntRows(db, "SELECT k FROM t ORDER BY k;", 1);
    ASSERT_EQ(rows.size(), static_cast<size_t>(kRows)) << "wrong row count on repeat " << rep;
    std::vector<int> got;
    got.reserve(rows.size());
    for (const auto &r : rows) {
      got.push_back(r[0]);
    }
    EXPECT_TRUE(std::is_sorted(got.begin(), got.end())) << "not ordered on repeat " << rep;
    EXPECT_EQ(got, expected) << "wrong contents on repeat " << rep;
  }
}

// Parallel UPDATE key permutation: every row's primary key is remapped to another row's key, with the
// rows split across many morsels/workers. Index maintenance is deferred to a single statement-wide
// Finalize (all old keys deleted before any new key inserted), so a per-batch scheme would false-conflict
// but this must succeed and stay unique.
TEST(ParallelExecutionTest, ParallelUpdateKeyPermutationSucceeds) {
  const int kRows = 3000;
  BumbleBeeInstance db;
  ConfigureParallel(db);
  NoopWriter noop;
  ASSERT_TRUE(db.ExecuteSql("CREATE TABLE p(id INT PRIMARY KEY, v INT);", noop));

  // Seed through the INSERT path (so the PK index is populated) with id == v == i.
  std::string ins = "INSERT INTO p VALUES ";
  for (int i = 0; i < kRows; i++) {
    if (i != 0) {
      ins += ",";
    }
    ins += "(" + std::to_string(i) + "," + std::to_string(i) + ")";
  }
  ins += ";";
  ASSERT_TRUE(db.ExecuteSql(ins, noop));

  // Reverse every key: id -> (kRows-1) - id. A full permutation across all parallel workers.
  ASSERT_TRUE(db.ExecuteSql("UPDATE p SET id = 2999 - id;", noop));

  // Every row survived, and specific rows moved to the expected mirrored key.
  auto cnt = RunIntRows(db, "SELECT COUNT(*) FROM p;", 1);
  ASSERT_EQ(cnt.size(), 1u);
  EXPECT_EQ(cnt[0][0], kRows);
  auto lo = RunIntRows(db, "SELECT id FROM p WHERE v = 0;", 1);
  ASSERT_EQ(lo.size(), 1u);
  EXPECT_EQ(lo[0][0], kRows - 1);
  auto hi = RunIntRows(db, "SELECT id FROM p WHERE v = 2999;", 1);
  ASSERT_EQ(hi.size(), 1u);
  EXPECT_EQ(hi[0][0], 0);

  // The keys are still unique/enforced after the permutation (id 0 is occupied by the mirror of 2999).
  EXPECT_THROW(db.ExecuteSql("INSERT INTO p VALUES (0, -1);", noop), Exception);
}

}  // namespace bumblebee
