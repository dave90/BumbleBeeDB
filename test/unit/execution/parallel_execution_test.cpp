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

namespace {

// SQL-created tables carry an auto BIGINT `_id` at column 0, so a (k, v) table is [_id, k, v]. We seed
// the heap directly (bypassing the INSERT executor) and fill `_id` with a sequential value.
const std::vector<LogicalType> kIdTwoInt{LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::INTEGER),
                                         LogicalType(LogicalTypeId::INTEGER)};

/** @brief Append `n` rows of (k = fk(i), v = fv(i)) into `name`'s heap, spanning many pages. */
void SeedHeap(BumbleBeeInstance &db, const std::string &name, int n, const std::function<int(int)> &fk,
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
void ConfigureParallel(BumbleBeeInstance &db) {
  db.max_threads_ = 8;   // clamped to [1, MAX_THREADS]; spawns this many query workers
  db.morsel_pages_ = 1;  // one page per morsel => many morsels => real parallel build
}

/** @brief Run `sql` and collect the result as `num_cols`-wide integer rows. */
auto RunIntRows(BumbleBeeInstance &db, const std::string &sql, int num_cols)
    -> std::vector<std::vector<int>> {
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

}  // namespace

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
