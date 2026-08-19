//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// prl_hash_table_test.cpp
//
// Identification: test/unit/execution/prl_hash_table_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/prl_hash_table.h"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "storage/row/row_layout.h"
#include "storage/row/row_operations.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

using OptInt = std::optional<int32_t>;
using OptStr = std::optional<std::string>;

static void SetIntCol(DataChunk &chunk, idx_t col, const std::vector<OptInt> &values) {
  for (idx_t i = 0; i < values.size(); i++) {
    chunk.SetValue(col, i, values[i].has_value() ? Value(*values[i]) : Value::Null(LogicalTypeId::INTEGER));
  }
}

static void SetStrCol(DataChunk &chunk, idx_t col, const std::vector<OptStr> &values) {
  for (idx_t i = 0; i < values.size(); i++) {
    chunk.SetValue(col, i, values[i].has_value() ? Value(*values[i]) : Value::Null(LogicalTypeId::STRING));
  }
}

static auto HashOf(DataChunk &keys) -> Vector {
  Vector hashes{LogicalType{LogicalTypeId::UBIGINT}, keys.GetSize()};
  keys.Hash(hashes);
  return hashes;
}

/** The (probe row -> matched key col-0 values) view of a probe result, for order-free comparison. */
static auto PairsOf(const PRLHashTable &ht, const std::vector<data_ptr_t> &addrs, const std::vector<sel_t> &rows,
                    idx_t payload_col) -> std::multiset<std::pair<sel_t, int32_t>> {
  std::multiset<std::pair<sel_t, int32_t>> out;
  SelectionVector identity;
  for (idx_t i = 0; i < addrs.size(); i++) {
    Vector row_vec{LogicalType{LogicalTypeId::UBIGINT},
                   reinterpret_cast<data_ptr_t>(const_cast<data_ptr_t *>(&addrs[i]))};
    Vector col{LogicalType{LogicalTypeId::INTEGER}, 1};
    RowOperations::Gather(ht.GetLayout(), row_vec, identity, col, identity, 1, payload_col);
    out.emplace(rows[i], col.GetValue(0).GetAs<int32_t>());
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------
// RowOperations::Gather / Match kernels
// ---------------------------------------------------------------------------------------------------

// Gather reads rows through row_sel, writes through col_sel, and NULL-pads a null row pointer — the
// exact contract the LEFT join's mixed matched/unmatched emission batches rely on.
TEST(RowOperationsGatherTest, SelectionsAndNullRowPointers) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::STRING};
  DataChunk in;
  in.Initialize(types);
  SetIntCol(in, 0, {1, 2});
  SetStrCol(in, 1, {std::string("short"), std::string("a long string that does not fit inline anywhere")});
  in.SetCardinality(2);

  RowLayout layout;
  layout.Initialize(types);
  std::vector<std::vector<char>> storage(2);
  storage[0].assign(layout.GetFixedRowWidth() + 5, 0);
  storage[1].assign(layout.GetFixedRowWidth() + 48, 0);
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  ptrs[0] = reinterpret_cast<data_ptr_t>(storage[0].data());
  ptrs[1] = reinterpret_cast<data_ptr_t>(storage[1].data());
  SelectionVector identity;
  RowOperations::Scatter(in, layout, rows, identity, 2);

  // Gather (row 1, null, row 0) into output positions (0, 1, 2).
  Vector gather_rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto gptrs = FlatVector::GetData<data_ptr_t>(gather_rows);
  gptrs[0] = ptrs[1];
  gptrs[1] = nullptr;
  gptrs[2] = ptrs[0];

  for (idx_t c = 0; c < types.size(); c++) {
    Vector col{types[c], 3};
    RowOperations::Gather(layout, gather_rows, identity, col, identity, 3, c);
    EXPECT_EQ(col.GetValue(0), in.GetValue(c, 1));
    EXPECT_TRUE(col.GetValue(1).IsNull());
    EXPECT_EQ(col.GetValue(2), in.GetValue(c, 0));
  }
}

// Match filters candidates column by column over the key prefix, with both NULL semantics: SQL '='
// (a NULL key never matches — joins) and IS NOT DISTINCT FROM (NULL == NULL — GROUP BY).
TEST(RowOperationsMatchTest, MultiColumnAndNullSemantics) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::STRING};
  DataChunk stored;
  stored.Initialize(types);
  SetIntCol(stored, 0, {1, 1, OptInt{}});
  SetStrCol(stored, 1, {std::string("aaaaaaaaaaaaaaaaX"), std::string("aaaaaaaaaaaaaaaaY"), OptStr{}});
  stored.SetCardinality(3);

  RowLayout layout;
  layout.Initialize(types);
  std::vector<std::vector<char>> storage(3);
  for (idx_t i = 0; i < 3; i++) {
    const idx_t len = stored.GetValue(1, i).IsNull() ? 0 : stored.GetValue(1, i).GetString().size();
    storage[i].assign(layout.GetFixedRowWidth() + len, 0);
  }
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < 3; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(storage[i].data());
  }
  SelectionVector identity;
  RowOperations::Scatter(stored, layout, rows, identity, 3);

  // Probe rows: (1, ...X) matches row 0 only (same 16-byte prefix as row 1 — the tail decides);
  // (NULL, NULL) matches row 2 only under IS NOT DISTINCT FROM.
  DataChunk probe;
  probe.Initialize(types);
  SetIntCol(probe, 0, {1, OptInt{}});
  SetStrCol(probe, 1, {std::string("aaaaaaaaaaaaaaaaX"), OptStr{}});
  probe.SetCardinality(2);
  auto col_data = probe.Orrify();

  // Candidates: every (probe row, stored row) combination.
  Vector cand_rows{LogicalType{LogicalTypeId::UBIGINT}};
  auto cptrs = FlatVector::GetData<data_ptr_t>(cand_rows);
  SelectionVector col_sel(6);
  for (idx_t p = 0; p < 2; p++) {
    for (idx_t s = 0; s < 3; s++) {
      cptrs[p * 3 + s] = ptrs[s];
      col_sel.SetIndex(p * 3 + s, p);
    }
  }

  SelectionVector match_sel(6);
  SelectionVector no_match_sel(6);
  idx_t no_match_count = 0;

  // Join semantics: only (probe 0, stored 0) survives; every NULL comparison fails.
  idx_t n = RowOperations::Match(probe, col_data.get(), layout, 2, cand_rows, identity, col_sel, 6, match_sel,
                                 no_match_sel, no_match_count, /*null_equal=*/false);
  ASSERT_EQ(n, 1U);
  EXPECT_EQ(match_sel.GetIndex(0), 0U);
  EXPECT_EQ(no_match_count, 5U);

  // GROUP BY semantics: (probe 1 = all-NULL) additionally matches the all-NULL stored row 2.
  no_match_count = 0;
  n = RowOperations::Match(probe, col_data.get(), layout, 2, cand_rows, identity, col_sel, 6, match_sel, no_match_sel,
                           no_match_count, /*null_equal=*/true);
  ASSERT_EQ(n, 2U);
  EXPECT_EQ(match_sel.GetIndex(0), 0U);
  EXPECT_EQ(match_sel.GetIndex(1), 5U);  // probe 1 x stored 2
}

// ---------------------------------------------------------------------------------------------------
// PRLHashTable — group mode (GROUP BY / DISTINCT semantics)
// ---------------------------------------------------------------------------------------------------

// Group mode dedups on the key prefix: re-adding the same keys finds the existing rows (same
// addresses), NULL keys collapse into one group, and new_group_sel reports creations in order.
TEST(PRLHashTableTest, GroupModeDedupsAndGroupsNulls) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER};
  PRLHashTable ht(types, 1, /*null_equal_keys=*/true);

  DataChunk groups;
  groups.Initialize(types);
  SetIntCol(groups, 0, {1, 2, 1, OptInt{}, OptInt{}, 2});
  groups.SetCardinality(6);

  auto hashes = HashOf(groups);
  Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, 6};
  SelectionVector new_sel(6);
  idx_t new_count = 0;
  ht.FindOrCreateGroups(hashes, groups, addresses, &new_sel, &new_count);

  // 1, 2, NULL -> three groups; duplicates and the second NULL matched existing rows.
  EXPECT_EQ(ht.Count(), 3U);
  EXPECT_EQ(new_count, 3U);
  EXPECT_EQ(new_sel.GetIndex(0), 0U);  // 1
  EXPECT_EQ(new_sel.GetIndex(1), 1U);  // 2
  EXPECT_EQ(new_sel.GetIndex(2), 3U);  // NULL
  auto addr = FlatVector::GetData<data_ptr_t>(addresses);
  EXPECT_EQ(addr[0], addr[2]);  // both 1s
  EXPECT_EQ(addr[1], addr[5]);  // both 2s
  EXPECT_EQ(addr[3], addr[4]);  // both NULLs
  EXPECT_NE(addr[0], addr[1]);

  // A second chunk with the same keys creates nothing.
  auto hashes2 = HashOf(groups);
  new_count = 99;
  ht.FindOrCreateGroups(hashes2, groups, addresses, &new_sel, &new_count);
  EXPECT_EQ(ht.Count(), 3U);
  EXPECT_EQ(new_count, 0U);
}

// String keys: two long strings sharing a 16-byte prefix stay distinct groups (the byte compare runs
// to the end), and the rows scan back intact — both with in-place and copied strings.
TEST(PRLHashTableTest, GroupModeStringKeysBeyondInlinePrefix) {
  std::vector<LogicalType> types{LogicalTypeId::STRING};
  PRLHashTable ht(types, 1, /*null_equal_keys=*/true);

  const std::string a = "aaaaaaaaaaaaaaaa-first";
  const std::string b = "aaaaaaaaaaaaaaaa-second";
  DataChunk groups;
  groups.Initialize(types);
  SetStrCol(groups, 0, {a, b, a, std::string("x"), OptStr{}});
  groups.SetCardinality(5);

  auto hashes = HashOf(groups);
  Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, 5};
  ht.FindOrCreateGroups(hashes, groups, addresses);
  EXPECT_EQ(ht.Count(), 4U);  // a, b, "x", NULL

  DataChunk out;
  out.Initialize(types);
  ASSERT_EQ(ht.Scan(0, out, /*copy_strings=*/true), 4U);
  std::multiset<std::string> got;
  idx_t nulls = 0;
  for (idx_t i = 0; i < 4; i++) {
    auto v = out.GetValue(0, i);
    if (v.IsNull()) {
      nulls++;
    } else {
      got.insert(v.GetString());
    }
  }
  EXPECT_EQ(nulls, 1U);
  EXPECT_EQ(got, (std::multiset<std::string>{a, b, "x"}));
}

// Growing far past the initial directory capacity forces multiple resizes; every group must survive
// the rehash and stay findable, and Scan must return them all exactly once, in insertion order.
TEST(PRLHashTableTest, GroupModeResizeKeepsEveryGroup) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::BIGINT};  // key + payload
  PRLHashTable ht(types, 1, /*null_equal_keys=*/true);

  constexpr idx_t kGroups = 5000;
  for (idx_t base = 0; base < kGroups; base += STANDARD_VECTOR_SIZE) {
    const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, kGroups - base);
    DataChunk groups;
    groups.Initialize(types);
    for (idx_t i = 0; i < n; i++) {
      groups.SetValue(0, i, Value(static_cast<int32_t>(base + i)));
      groups.SetValue(1, i, Value(static_cast<int64_t>(base + i) * 10));
    }
    groups.SetCardinality(n);
    DataChunk keys;
    keys.InitializeEmpty({types[0]});
    keys.data_[0].Reference(groups.data_[0]);
    keys.SetCardinality(n);
    auto hashes = HashOf(keys);
    Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, n};
    ht.FindOrCreateGroups(hashes, groups, addresses);
  }
  ASSERT_EQ(ht.Count(), kGroups);

  // Scan back in insertion order, key i at position i with its payload.
  DataChunk out;
  out.Initialize(types);
  idx_t offset = 0;
  while (idx_t n = ht.Scan(offset, out)) {
    for (idx_t i = 0; i < n; i++) {
      ASSERT_EQ(out.GetValue(0, i).GetAs<int32_t>(), static_cast<int32_t>(offset + i));
      ASSERT_EQ(out.GetValue(1, i).GetAs<int64_t>(), static_cast<int64_t>(offset + i) * 10);
    }
    offset += n;
    out.Reset();
  }
  EXPECT_EQ(offset, kGroups);
}

// ---------------------------------------------------------------------------------------------------
// PRLHashTable — join mode (Append + Probe, bag semantics)
// ---------------------------------------------------------------------------------------------------

// Append keeps duplicate keys as separate rows; Probe returns the full cross product per key, in
// probe-row order, and reports per-row matched flags (the LEFT join's NULL-padding signal).
TEST(PRLHashTableTest, AppendKeepsDuplicatesAndProbeCrossProduct) {
  // Layout: key INTEGER + payload INTEGER (the "build column").
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::INTEGER};
  PRLHashTable ht(types, 1, /*null_equal_keys=*/false);

  DataChunk build;
  build.Initialize(types);
  SetIntCol(build, 0, {1, 1, 2, 3});      // key 1 twice
  SetIntCol(build, 1, {10, 11, 20, 30});  // payloads
  build.SetCardinality(4);
  DataChunk keys;
  keys.InitializeEmpty({types[0]});
  keys.data_[0].Reference(build.data_[0]);
  keys.SetCardinality(4);
  auto hashes = HashOf(keys);
  SelectionVector identity;
  ht.Append(hashes, build, identity, 4);
  EXPECT_EQ(ht.Count(), 4U);  // no dedup

  // Probe: key 1 (2 matches), key 4 (none), key 2 (1 match).
  DataChunk probe;
  probe.Initialize({types[0]});
  SetIntCol(probe, 0, {1, 4, 2});
  probe.SetCardinality(3);
  auto probe_hashes = HashOf(probe);

  std::vector<data_ptr_t> addrs;
  std::vector<sel_t> rows;
  std::vector<uint8_t> matched(3, 0);
  ht.Probe(probe_hashes, probe, identity, 3, addrs, rows, &matched);

  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(PairsOf(ht, addrs, rows, 1), (std::multiset<std::pair<sel_t, int32_t>>{{0, 10}, {0, 11}, {2, 20}}));
  EXPECT_EQ(matched, (std::vector<uint8_t>{1, 0, 1}));
  // Probe-row order: all of row 0's matches precede row 2's.
  EXPECT_TRUE(rows[0] == 0 && rows[1] == 0 && rows[2] == 2);
}

// Join semantics never match NULL keys: a NULL-keyed build row is unreachable even by a NULL-keyed
// probe row (their hashes collide — the Match kernel must reject the pair).
TEST(PRLHashTableTest, ProbeNullKeysNeverJoin) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::INTEGER};
  PRLHashTable ht(types, 1, /*null_equal_keys=*/false);

  DataChunk build;
  build.Initialize(types);
  SetIntCol(build, 0, {OptInt{}, 7});
  SetIntCol(build, 1, {100, 700});
  build.SetCardinality(2);
  DataChunk keys;
  keys.InitializeEmpty({types[0]});
  keys.data_[0].Reference(build.data_[0]);
  keys.SetCardinality(2);
  auto hashes = HashOf(keys);
  SelectionVector identity;
  ht.Append(hashes, build, identity, 2);

  DataChunk probe;
  probe.Initialize({types[0]});
  SetIntCol(probe, 0, {OptInt{}, 7});
  probe.SetCardinality(2);
  auto probe_hashes = HashOf(probe);

  std::vector<data_ptr_t> addrs;
  std::vector<sel_t> rows;
  ht.Probe(probe_hashes, probe, identity, 2, addrs, rows);
  ASSERT_EQ(rows.size(), 1U);  // only 7 = 7
  EXPECT_EQ(rows[0], 1U);
}

// Multi-column keys filter progressively: rows equal on the first key column but different on the
// second must not match, including when the difference is a NULL.
TEST(PRLHashTableTest, MultiColumnKeysPartialEquality) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::STRING, LogicalTypeId::INTEGER};
  PRLHashTable ht(types, 2, /*null_equal_keys=*/false);

  DataChunk build;
  build.Initialize(types);
  SetIntCol(build, 0, {1, 1, 1});
  SetStrCol(build, 1, {std::string("x"), std::string("y"), OptStr{}});
  SetIntCol(build, 2, {10, 11, 12});
  build.SetCardinality(3);
  DataChunk keys;
  keys.InitializeEmpty({types[0], types[1]});
  keys.data_[0].Reference(build.data_[0]);
  keys.data_[1].Reference(build.data_[1]);
  keys.SetCardinality(3);
  auto hashes = HashOf(keys);
  SelectionVector identity;
  ht.Append(hashes, build, identity, 3);

  DataChunk probe;
  probe.Initialize({types[0], types[1]});
  SetIntCol(probe, 0, {1, 1});
  SetStrCol(probe, 1, {std::string("y"), OptStr{}});
  probe.SetCardinality(2);
  auto probe_hashes = HashOf(probe);

  std::vector<data_ptr_t> addrs;
  std::vector<sel_t> rows;
  ht.Probe(probe_hashes, probe, identity, 2, addrs, rows);
  ASSERT_EQ(rows.size(), 1U);  // only (1, "y"); (1, NULL) matches nothing
  EXPECT_EQ(rows[0], 0U);
  EXPECT_EQ(PairsOf(ht, addrs, rows, 2), (std::multiset<std::pair<sel_t, int32_t>>{{0, 11}}));
}

// The parallel-build path: rows scattered into separate "thread-local" tables with AppendUnbuilt,
// spliced together with Merge (blocks move, rows don't), then one BuildDirectory pass. A probe must
// find every row — including a key whose duplicates were split across the local tables — and the
// merged table must keep accepting new rows (the bump allocator must not clobber a stolen block).
TEST(PRLHashTableTest, AppendUnbuiltMergeBuildDirectory) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::INTEGER};
  PRLHashTable global(types, 1, /*null_equal_keys=*/false);
  SelectionVector identity;

  // Two "sink tasks": key 7 gets one duplicate in each local table.
  for (int task = 0; task < 2; task++) {
    PRLHashTable local(types, 1, /*null_equal_keys=*/false);
    DataChunk build;
    build.Initialize(types);
    SetIntCol(build, 0, {7, 100 + task});
    SetIntCol(build, 1, {task, 100 + task});
    build.SetCardinality(2);
    DataChunk keys;
    keys.InitializeEmpty({types[0]});
    keys.data_[0].Reference(build.data_[0]);
    keys.SetCardinality(2);
    auto hashes = HashOf(keys);
    local.AppendUnbuilt(hashes, build, identity, 2);
    global.Merge(local);
    EXPECT_EQ(local.Count(), 0U);  // spliced away
  }
  ASSERT_EQ(global.Count(), 4U);
  global.BuildDirectory();

  DataChunk probe;
  probe.Initialize({types[0]});
  SetIntCol(probe, 0, {7, 100, 101, 9});
  probe.SetCardinality(4);
  auto probe_hashes = HashOf(probe);
  std::vector<data_ptr_t> addrs;
  std::vector<sel_t> rows;
  global.Probe(probe_hashes, probe, identity, 4, addrs, rows);
  EXPECT_EQ(PairsOf(global, addrs, rows, 1),
            (std::multiset<std::pair<sel_t, int32_t>>{{0, 0}, {0, 1}, {1, 100}, {2, 101}}));

  // Post-merge growth: append more rows (fresh block) and rebuild — everything stays findable.
  DataChunk more;
  more.Initialize(types);
  SetIntCol(more, 0, {7});
  SetIntCol(more, 1, {2});
  more.SetCardinality(1);
  DataChunk more_keys;
  more_keys.InitializeEmpty({types[0]});
  more_keys.data_[0].Reference(more.data_[0]);
  more_keys.SetCardinality(1);
  auto more_hashes = HashOf(more_keys);
  global.AppendUnbuilt(more_hashes, more, identity, 1);
  global.BuildDirectory();

  addrs.clear();
  rows.clear();
  auto probe_hashes2 = HashOf(probe);
  global.Probe(probe_hashes2, probe, identity, 4, addrs, rows);
  EXPECT_EQ(PairsOf(global, addrs, rows, 1),
            (std::multiset<std::pair<sel_t, int32_t>>{{0, 0}, {0, 1}, {0, 2}, {1, 100}, {2, 101}}));
}

// A build far larger than the initial directory forces resizes mid-append; a full probe of every key
// must still find every duplicate (the candidate walk spans rehashed clusters).
TEST(PRLHashTableTest, AppendResizeThenProbeFindsEverything) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::INTEGER};
  PRLHashTable ht(types, 1, /*null_equal_keys=*/false);

  constexpr idx_t kKeys = 3000;  // x2 duplicates = 6000 rows over several chunks
  SelectionVector identity;
  for (int round = 0; round < 2; round++) {
    for (idx_t base = 0; base < kKeys; base += STANDARD_VECTOR_SIZE) {
      const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, kKeys - base);
      DataChunk build;
      build.Initialize(types);
      for (idx_t i = 0; i < n; i++) {
        build.SetValue(0, i, Value(static_cast<int32_t>(base + i)));
        build.SetValue(1, i, Value(static_cast<int32_t>(round)));
      }
      build.SetCardinality(n);
      DataChunk keys;
      keys.InitializeEmpty({types[0]});
      keys.data_[0].Reference(build.data_[0]);
      keys.SetCardinality(n);
      auto hashes = HashOf(keys);
      ht.Append(hashes, build, identity, n);
    }
  }
  ASSERT_EQ(ht.Count(), 2 * kKeys);

  idx_t total_matches = 0;
  for (idx_t base = 0; base < kKeys; base += STANDARD_VECTOR_SIZE) {
    const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, kKeys - base);
    DataChunk probe;
    probe.Initialize({types[0]});
    for (idx_t i = 0; i < n; i++) {
      probe.SetValue(0, i, Value(static_cast<int32_t>(base + i)));
    }
    probe.SetCardinality(n);
    auto probe_hashes = HashOf(probe);
    std::vector<data_ptr_t> addrs;
    std::vector<sel_t> rows;
    ht.Probe(probe_hashes, probe, identity, n, addrs, rows);
    ASSERT_EQ(rows.size(), 2 * n);  // every key has exactly its two duplicates
    total_matches += rows.size();
  }
  EXPECT_EQ(total_matches, 2 * kKeys);
}

}  // namespace bumblebee
