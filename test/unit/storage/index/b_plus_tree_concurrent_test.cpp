//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_concurrent_test.cpp
//
// Identification: test/unit/storage/index/b_plus_tree_concurrent_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "concurrency_test_util.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/index/b_plus_tree.h"
#include "storage/index/generic_key.h"
#include "storage/table/rid.h"

namespace bumblebee {

using Key = GenericKey<8>;
using Cmp = GenericComparator<8>;
using Tree = BPlusTree<Key, RID, Cmp>;

static auto MakeKey(int64_t k) -> Key {
  Key key;
  key.SetFromInteger(k);
  return key;
}

/**
 * @brief A tree sized for the concurrent tests: a bigger pool and fanout 5 than the
 *        single-threaded fixture in b_plus_tree_test.cpp uses.
 *
 * Deliberately NOT named `TreeFixture`: both files live in namespace `bumblebee` and link into the
 * same `unit_tests` binary, so sharing the name would be an ODR violation between two structs that
 * really do differ, and the linker would silently pick one constructor for both.
 */
struct ConcurrentTreeFixture {
  MemoryDiskManager dm;
  BufferPoolManager bpm;
  Schema key_schema;
  Cmp cmp;
  page_id_t header_page_id;
  std::unique_ptr<Tree> tree;

  ConcurrentTreeFixture()
      : dm(8192),
        bpm(128, &dm),
        key_schema(std::vector<Column>{Column("k", LogicalType(LogicalTypeId::BIGINT))}),
        cmp(&key_schema),
        header_page_id(bpm.NewPage()) {
    tree = std::make_unique<Tree>("idx", header_page_id, &bpm, cmp, 5, 5);
  }
};

// N threads insert DISJOINT key subsets (partitioned by key % threads). Concurrent latch-crabbing
// must lose no key. TSan-clean = no latch leak / deadlock / race.
TEST(BPlusTreeConcurrentTest, ConcurrentInsertDisjoint) {
  ConcurrentTreeFixture f;
  const int n = 500;
  const int threads = 4;
  LaunchParallelTest(threads, [&](uint64_t tid) {
    for (int k = 0; k < n; k++) {
      if (k % threads == static_cast<int>(tid)) {
        f.tree->Insert(MakeKey(k), RID(k, k));
      }
    }
  });

  for (int k = 0; k < n; k++) {
    std::vector<RID> res;
    ASSERT_TRUE(f.tree->GetValue(MakeKey(k), &res)) << "missing key " << k;
    EXPECT_EQ(res[0].Get(), RID(k, k).Get());
  }
}

// Concurrent lookups while a single writer inserts: readers must never crash and either see the key
// or not (no torn reads). Then everything is present.
TEST(BPlusTreeConcurrentTest, ConcurrentInsertAndLookup) {
  ConcurrentTreeFixture f;
  const int n = 400;
  // Two inserter threads on disjoint halves + two lookup threads scanning the whole range.
  LaunchParallelTest(4, [&](uint64_t tid) {
    if (tid < 2) {
      for (int k = 0; k < n; k++) {
        if (k % 2 == static_cast<int>(tid)) {
          f.tree->Insert(MakeKey(k), RID(k, k));
        }
      }
    } else {
      for (int rep = 0; rep < 5; rep++) {
        for (int k = 0; k < n; k++) {
          std::vector<RID> res;
          f.tree->GetValue(MakeKey(k), &res);  // may or may not be present yet; must not crash
        }
      }
    }
  });

  for (int k = 0; k < n; k++) {
    std::vector<RID> res;
    ASSERT_TRUE(f.tree->GetValue(MakeKey(k), &res)) << "missing key " << k;
  }
}

// Concurrent deletes of disjoint subsets after a full insert; the complementary keys survive.
TEST(BPlusTreeConcurrentTest, ConcurrentDeleteDisjoint) {
  ConcurrentTreeFixture f;
  const int n = 500;
  const int threads = 4;
  for (int k = 0; k < n; k++) {
    ASSERT_TRUE(f.tree->Insert(MakeKey(k), RID(k, k)));
  }
  // Each thread deletes keys where key % (2*threads) == tid (so keys with residue >= threads survive).
  LaunchParallelTest(threads, [&](uint64_t tid) {
    for (int k = 0; k < n; k++) {
      if (k % (2 * threads) == static_cast<int>(tid)) {
        f.tree->Remove(MakeKey(k));
      }
    }
  });

  for (int k = 0; k < n; k++) {
    std::vector<RID> res;
    bool deleted = (k % (2 * threads)) < threads;
    EXPECT_EQ(f.tree->GetValue(MakeKey(k), &res), !deleted) << "key " << k;
  }
}

}  // namespace bumblebee
