//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_test.cpp
//
// Identification: test/unit/storage/index/b_plus_tree_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/index/b_plus_tree.h"

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
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

struct TreeFixture {
  MemoryDiskManager dm;
  BufferPoolManager bpm;
  Schema key_schema;
  Cmp cmp;
  page_id_t header_page_id;
  std::unique_ptr<Tree> tree;

  explicit TreeFixture(int leaf_max = 4, int internal_max = 4)
      : dm(4096),
        bpm(64, &dm),
        key_schema(std::vector<Column>{Column("k", LogicalType(LogicalTypeId::BIGINT))}),
        cmp(&key_schema),
        header_page_id(bpm.NewPage()) {
    tree = std::make_unique<Tree>("idx", header_page_id, &bpm, cmp, leaf_max, internal_max);
  }
};

TEST(BPlusTreeTest, InsertLookupAndDuplicate) {
  TreeFixture f;
  EXPECT_TRUE(f.tree->IsEmpty());
  for (int i = 0; i < 20; i++) {
    EXPECT_TRUE(f.tree->Insert(MakeKey(i), RID(i, i)));
  }
  EXPECT_FALSE(f.tree->IsEmpty());
  EXPECT_FALSE(f.tree->Insert(MakeKey(5), RID(99, 99)));  // duplicate

  for (int i = 0; i < 20; i++) {
    std::vector<RID> res;
    ASSERT_TRUE(f.tree->GetValue(MakeKey(i), &res));
    ASSERT_EQ(res.size(), 1U);
    EXPECT_EQ(res[0].Get(), RID(i, i).Get());
  }
  std::vector<RID> miss;
  EXPECT_FALSE(f.tree->GetValue(MakeKey(1000), &miss));
}

// Forces multiple leaf and internal splits (small node sizes, ascending keys).
TEST(BPlusTreeTest, ForceSplitsKeepsAllKeys) {
  TreeFixture f(/*leaf_max=*/4, /*internal_max=*/4);
  const int n = 200;
  for (int i = 0; i < n; i++) {
    ASSERT_TRUE(f.tree->Insert(MakeKey(i), RID(i, i)));
  }
  for (int i = 0; i < n; i++) {
    std::vector<RID> res;
    ASSERT_TRUE(f.tree->GetValue(MakeKey(i), &res)) << "missing key " << i;
    EXPECT_EQ(res[0].Get(), RID(i, i).Get());
  }
}

TEST(BPlusTreeTest, IteratorVisitsKeysInOrder) {
  TreeFixture f;
  std::vector<int> keys;
  for (int i = 0; i < 50; i++) {
    keys.push_back(i);
  }
  std::mt19937 rng(42);
  auto shuffled = keys;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  for (auto k : shuffled) {
    ASSERT_TRUE(f.tree->Insert(MakeKey(k), RID(k, k)));
  }

  std::vector<int64_t> seen;
  for (auto it = f.tree->Begin(); !it.IsEnd(); ++it) {
    seen.push_back((*it).first.GetAsInteger());
  }
  ASSERT_EQ(seen.size(), keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    EXPECT_EQ(seen[i], keys[i]);
  }

  // Range scan from a key.
  std::vector<int64_t> from20;
  for (auto it = f.tree->Begin(MakeKey(20)); !it.IsEnd(); ++it) {
    from20.push_back((*it).first.GetAsInteger());
  }
  ASSERT_FALSE(from20.empty());
  EXPECT_EQ(from20.front(), 20);
  EXPECT_EQ(from20.size(), 30U);
}

// Deletes with redistribute/merge; then delete everything and confirm the tree collapses to empty.
TEST(BPlusTreeTest, DeleteRedistributeMergeAndCollapse) {
  TreeFixture f(/*leaf_max=*/4, /*internal_max=*/4);
  const int n = 100;
  for (int i = 0; i < n; i++) {
    ASSERT_TRUE(f.tree->Insert(MakeKey(i), RID(i, i)));
  }
  // Remove the even keys; the odd keys must all still be found.
  for (int i = 0; i < n; i += 2) {
    f.tree->Remove(MakeKey(i));
  }
  for (int i = 0; i < n; i++) {
    std::vector<RID> res;
    if (i % 2 == 0) {
      EXPECT_FALSE(f.tree->GetValue(MakeKey(i), &res)) << "deleted key " << i << " still present";
    } else {
      ASSERT_TRUE(f.tree->GetValue(MakeKey(i), &res)) << "surviving key " << i << " missing";
      EXPECT_EQ(res[0].Get(), RID(i, i).Get());
    }
  }
  // Remove the rest → empty tree, root collapsed to INVALID.
  for (int i = 1; i < n; i += 2) {
    f.tree->Remove(MakeKey(i));
  }
  EXPECT_TRUE(f.tree->IsEmpty());
  EXPECT_EQ(f.tree->GetRootPageId(), INVALID_PAGE_ID);
  std::vector<RID> res;
  EXPECT_FALSE(f.tree->GetValue(MakeKey(3), &res));
}

// The optimistic-delete root-collapse fix: a single leaf-root (no splits) drained to empty must
// reset the root to INVALID_PAGE_ID (the emptying delete bails to the pessimistic collapse path),
// never leaving a dangling empty leaf as the root.
TEST(BPlusTreeTest, OptimisticDeleteCollapsesLeafRoot) {
  TreeFixture f(/*leaf_max=*/10, /*internal_max=*/10);  // large enough that 3 keys stay one leaf
  for (int i = 1; i <= 3; i++) {
    ASSERT_TRUE(f.tree->Insert(MakeKey(i), RID(i, i)));
  }
  EXPECT_NE(f.tree->GetRootPageId(), INVALID_PAGE_ID);
  for (int i = 1; i <= 3; i++) {
    f.tree->Remove(MakeKey(i));  // the 3rd (last) delete drives the leaf-root to empty
  }
  EXPECT_TRUE(f.tree->IsEmpty());
  EXPECT_EQ(f.tree->GetRootPageId(), INVALID_PAGE_ID) << "emptied leaf-root must collapse to INVALID";

  // The tree is reusable: inserting again rebuilds a root.
  ASSERT_TRUE(f.tree->Insert(MakeKey(42), RID(42, 42)));
  std::vector<RID> res;
  ASSERT_TRUE(f.tree->GetValue(MakeKey(42), &res));
  EXPECT_EQ(res[0].Get(), RID(42, 42).Get());
}

TEST(BPlusTreeTest, RandomizedInsertDelete) {
  TreeFixture f(/*leaf_max=*/5, /*internal_max=*/5);
  std::mt19937 rng(7);
  const int n = 300;
  std::vector<int> keys(n);
  for (int i = 0; i < n; i++) {
    keys[i] = i;
  }
  std::shuffle(keys.begin(), keys.end(), rng);
  for (auto k : keys) {
    ASSERT_TRUE(f.tree->Insert(MakeKey(k), RID(k, k)));
  }
  // Delete a random half.
  std::shuffle(keys.begin(), keys.end(), rng);
  std::vector<bool> deleted(n, false);
  for (int i = 0; i < n / 2; i++) {
    f.tree->Remove(MakeKey(keys[i]));
    deleted[keys[i]] = true;
  }
  for (int k = 0; k < n; k++) {
    std::vector<RID> res;
    EXPECT_EQ(f.tree->GetValue(MakeKey(k), &res), !deleted[k]) << "key " << k;
  }
}

}  // namespace bumblebee
