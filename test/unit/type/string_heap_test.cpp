//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_heap_test.cpp
//
// Identification: test/unit/type/string_heap_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <string>

#include "common/config.h"
#include "gtest/gtest.h"
#include "type/string_heap.h"

namespace bumblebee {

class StringHeapTest : public ::testing::Test {
 protected:
  static constexpr idx_t CHUNK_SIZE = MINIMUM_HEAP_SIZE;
  StringHeap heap_;
};

auto CompareStringT(string_t &s1, const char *s2) -> int { return strncmp(s1.CStr(), s2, s1.Length()); }

TEST_F(StringHeapTest, AddCStringWithLength) {
  const char *data = "HelloHelloHello";
  string_t result = heap_.AddString(data, strlen(data));
  EXPECT_EQ(CompareStringT(result, "HelloHelloHello"), 0);
}

TEST_F(StringHeapTest, AddCStringWithoutLength) {
  const char *data = "WorldWorldWorldWorldWorld";
  string_t result = heap_.AddString(data);
  EXPECT_EQ(CompareStringT(result, "WorldWorldWorldWorldWorld"), 0);
}

TEST_F(StringHeapTest, AddStdString) {
  std::string data = "MiaoMiaoMiaoMiaoMiaoMiao";
  string_t result = heap_.AddString(data);
  EXPECT_EQ(CompareStringT(result, "MiaoMiaoMiaoMiaoMiaoMiao"), 0);
}

TEST_F(StringHeapTest, AddStringT) {
  string_t data = "BumbleBee";
  string_t result = heap_.AddString(data);
  EXPECT_EQ(CompareStringT(result, "BumbleBee"), 0);
}

TEST_F(StringHeapTest, AddEmptyString) {
  string_t result = heap_.AddEmptyString(200);
  EXPECT_EQ(result.Length(), 200);
  EXPECT_EQ(CompareStringT(result, ""), 0);
}

TEST_F(StringHeapTest, DestroyDoesNotCrash) {
  heap_.AddString("Before Destroyyy");
  heap_.Destroy();
  string_t result = heap_.AddString("After Destroyy");
  EXPECT_EQ(CompareStringT(result, "After Destroyy"), 0);
}

TEST_F(StringHeapTest, SingleChunkAllocation) {
  // Insert strings that fit in a single chunk (note: these are not inlined strings).
  auto s1 = heap_.AddEmptyString(100);
  s1.GetDataWriteable()[0] = 'A';
  auto s2 = heap_.AddEmptyString(200);
  auto s3 = heap_.AddEmptyString(300);
  auto s4 = heap_.AddEmptyString(CHUNK_SIZE - 700);
  // All of them should be in the same chunk (no overflow).
  const auto *base = s1.CStr();
  EXPECT_EQ(s2.CStr(), base + 101);  // +1 for the null termination allocation
  EXPECT_EQ(s3.CStr(), base + 101 + 201);
  EXPECT_EQ(s4.CStr(), base + 101 + 201 + 301);

  // This one causes an overflow and allocates a new chunk.
  auto s5 = heap_.AddEmptyString(101);
  EXPECT_NE(s5.CStr(), base + 101 + 201 + 301 + (CHUNK_SIZE - 700 + 1));
  // Check that s1's first chunk still exists.
  EXPECT_EQ(s1.CStr()[0], 'A');
  heap_.Destroy();
}

}  // namespace bumblebee
