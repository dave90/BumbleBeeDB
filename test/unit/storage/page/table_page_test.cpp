//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_page_test.cpp
//
// Identification: test/unit/storage/page/table_page_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/page/table_page.h"

#include <cstring>
#include <vector>

#include "common/config.h"
#include "common/exception.h"
#include "gtest/gtest.h"

namespace bumblebee {

static auto MakePage() -> std::vector<char> {
  std::vector<char> backing(PAGE_SIZE, 0);
  reinterpret_cast<TablePage *>(backing.data())->Init();
  return backing;
}

/** @brief View a byte buffer as RowLayout bytes for the TablePage API. */
static auto AsRow(const std::vector<char> &v) -> const_data_ptr_t {
  return reinterpret_cast<const_data_ptr_t>(v.data());
}

TEST(TablePageTest, InsertAndReadRows) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());

  std::vector<char> row = {'r', 'o', 'w', '1'};
  TupleMeta meta{0, false};
  auto slot = page->InsertRow(meta, AsRow(row), row.size());
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 0U);
  EXPECT_EQ(page->GetNumTuples(), 1U);

  auto [got_meta, ptr, size] = page->GetRow(*slot);
  EXPECT_EQ(size, row.size());
  EXPECT_EQ(0, std::memcmp(ptr, row.data(), row.size()));
  EXPECT_FALSE(got_meta.is_deleted_);
}

// Resizing a row via UpdateRow keeps every slot number stable and preserves the neighbouring rows'
// bytes, growing and shrinking by compacting the page in place.
TEST(TablePageTest, UpdateRowResizesAndPreservesNeighbours) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());

  // Three rows, inserted in order: slot 0 (top of page), slot 1, slot 2 (bottom).
  std::vector<char> a(8, 'a');
  std::vector<char> b(8, 'b');
  std::vector<char> c(8, 'c');
  auto s0 = page->InsertRow(TupleMeta{0, false}, AsRow(a), a.size());
  auto s1 = page->InsertRow(TupleMeta{0, false}, AsRow(b), b.size());
  auto s2 = page->InsertRow(TupleMeta{0, false}, AsRow(c), c.size());
  ASSERT_TRUE(s0 && s1 && s2);

  auto row_bytes = [&](uint16_t slot) {
    auto [meta, ptr, size] = page->GetRow(slot);
    return std::string(ptr, ptr + size);
  };

  // Grow the middle row (slot 1): the rows below it (slot 2) must shift but keep their bytes.
  std::vector<char> big(64, 'B');
  ASSERT_TRUE(page->UpdateRow(TupleMeta{0, false}, AsRow(big), big.size(), *s1));
  EXPECT_EQ(row_bytes(*s0), std::string(8, 'a')) << "row above is untouched";
  EXPECT_EQ(row_bytes(*s1), std::string(64, 'B')) << "grown row";
  EXPECT_EQ(row_bytes(*s2), std::string(8, 'c')) << "row below shifted but intact";

  // Shrink it again to a tiny row; neighbours still intact.
  std::vector<char> tiny(2, 'T');
  ASSERT_TRUE(page->UpdateRow(TupleMeta{0, false}, AsRow(tiny), tiny.size(), *s1));
  EXPECT_EQ(row_bytes(*s0), std::string(8, 'a'));
  EXPECT_EQ(row_bytes(*s1), std::string(2, 'T'));
  EXPECT_EQ(row_bytes(*s2), std::string(8, 'c'));
}

// A grow that cannot fit the page's free space is rejected (returns false), leaving the page usable.
TEST(TablePageTest, UpdateRowRejectsRowTooLargeForPage) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());
  std::vector<char> row(16, 'x');
  auto slot = page->InsertRow(TupleMeta{0, false}, AsRow(row), row.size());
  ASSERT_TRUE(slot.has_value());

  std::vector<char> huge(PAGE_SIZE, 'z');  // cannot possibly fit
  EXPECT_FALSE(page->UpdateRow(TupleMeta{0, false}, AsRow(huge), huge.size(), *slot));
  // The original row survives the rejected update.
  auto [meta, ptr, size] = page->GetRow(*slot);
  EXPECT_EQ(size, 16U);
  EXPECT_EQ(0, std::memcmp(ptr, row.data(), 16));
}

TEST(TablePageTest, DeleteMarksMetaAndCountsIt) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());
  std::vector<char> row(16, 'x');
  auto slot = page->InsertRow(TupleMeta{0, false}, AsRow(row), row.size());
  ASSERT_TRUE(slot.has_value());

  page->UpdateTupleMeta(TupleMeta{0, true}, *slot);
  EXPECT_TRUE(page->GetTupleMeta(*slot).is_deleted_);
  EXPECT_EQ(page->GetNumDeletedTuples(), 1U);
  // The slot still exists (stable slot numbers).
  EXPECT_EQ(page->GetNumTuples(), 1U);
}

// Bug #1: a row larger than the page (or the remaining space) must return nullopt, never a wrapped
// offset that would memcpy out of bounds. ASan proves no OOB write.
TEST(TablePageTest, OversizedRowIsRejected) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());

  std::vector<char> huge(PAGE_SIZE + 100, 'z');
  auto slot = page->InsertRow(TupleMeta{0, false}, AsRow(huge), static_cast<uint16_t>(huge.size()));
  EXPECT_FALSE(slot.has_value());
  EXPECT_EQ(page->GetNumTuples(), 0U);  // page untouched
}

TEST(TablePageTest, FillsUpThenRejects) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());

  // Each row: 200 bytes payload + 24 bytes slot => ~224 bytes/row; the page holds a bounded number.
  std::vector<char> row(200, 'a');
  int inserted = 0;
  while (page->InsertRow(TupleMeta{0, false}, AsRow(row), row.size()).has_value()) {
    inserted++;
  }
  EXPECT_GT(inserted, 0);
  EXPECT_EQ(static_cast<int>(page->GetNumTuples()), inserted);
  // No more space: further inserts keep failing.
  EXPECT_FALSE(page->InsertRow(TupleMeta{0, false}, AsRow(row), row.size()).has_value());
}

TEST(TablePageTest, UpdateInPlaceRequiresSameSize) {
  auto backing = MakePage();
  auto *page = reinterpret_cast<TablePage *>(backing.data());
  std::vector<char> row(10, 'a');
  auto slot = page->InsertRow(TupleMeta{0, false}, AsRow(row), row.size());
  ASSERT_TRUE(slot.has_value());

  std::vector<char> same(10, 'b');
  page->UpdateRowInPlaceUnsafe(TupleMeta{0, false}, AsRow(same), same.size(), *slot);
  auto [meta, ptr, size] = page->GetRow(*slot);
  EXPECT_EQ(ptr[0], 'b');

  std::vector<char> bigger(11, 'c');
  EXPECT_THROW(page->UpdateRowInPlaceUnsafe(TupleMeta{0, false}, AsRow(bigger), bigger.size(), *slot), Exception);
}

}  // namespace bumblebee
