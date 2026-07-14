//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// chunk_collection_test.cpp
//
// Identification: test/unit/type/vector/chunk_collection_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/chunk_collection.h"

#include <vector>

#include "gtest/gtest.h"
#include "type/value.h"

namespace bumblebee {

class ChunkCollectionTest : public ::testing::Test {
 protected:
  ChunkCollection collection_;

  std::vector<PhysicalType> test_types_{PhysicalType::INTEGER, PhysicalType::UINTEGER, PhysicalType::BIGINT};

  /**
   * @brief A chunk of (INTEGER, UINTEGER, BIGINT) whose row i is (i, i * 10, i * 100).
   *
   * @param count The number of rows.
   * @return DataChunk The chunk.
   */
  auto CreateChunkWithValue(idx_t count = 1) -> DataChunk {
    DataChunk chunk;
    chunk.Initialize(test_types_);
    chunk.SetCardinality(count);
    for (idx_t i = 0; i < count; ++i) {
      chunk.SetValue(0, i, Value(static_cast<int32_t>(i)));
      chunk.SetValue(1, i, Value(static_cast<uint32_t>(i * 10)));
      chunk.SetValue(2, i, Value(static_cast<int64_t>(i * 100)));
    }
    chunk.SetCardinality(count);
    return chunk;
  }
};

TEST_F(ChunkCollectionTest, InitialState) {
  EXPECT_EQ(collection_.GetCount(), 0);
  EXPECT_EQ(collection_.ColumnCount(), 0);
  EXPECT_EQ(collection_.ChunkCount(), 0);
  EXPECT_TRUE(collection_.GetTypes().empty());
  EXPECT_EQ(collection_.ToString(), "ChunkCollection [ 0 ]");
}

TEST_F(ChunkCollectionTest, AppendSingleChunk) {
  DataChunk chunk = CreateChunkWithValue(3);
  collection_.Append(chunk);

  EXPECT_EQ(collection_.GetCount(), 3);
  EXPECT_EQ(collection_.ColumnCount(), 3);
  EXPECT_EQ(collection_.ChunkCount(), 1);
  for (idx_t i = 0; i < 3; ++i) {
    EXPECT_EQ(collection_.GetValue(0, i), Value(static_cast<int32_t>(i)));
    EXPECT_EQ(collection_.GetValue(1, i), Value(static_cast<uint32_t>(i * 10)));
    EXPECT_EQ(collection_.GetValue(2, i), Value(static_cast<int64_t>(i * 100)));
  }
}

TEST_F(ChunkCollectionTest, AppendMultipleChunks) {
  for (int i = 0; i < 2; ++i) {
    DataChunk chunk = CreateChunkWithValue(2);
    collection_.Append(chunk);
  }
  EXPECT_EQ(collection_.GetCount(), 4);
  EXPECT_EQ(collection_.ChunkCount(), 1);
}

TEST_F(ChunkCollectionTest, AppendOnEmptyCollection) {
  auto chunk1 = CreateChunkWithValue(3);
  auto chunk2 = CreateChunkWithValue(3);
  auto chunk3 = CreateChunkWithValue(3);

  ChunkCollection other;
  other.Append(chunk1);
  other.Append(chunk2);
  other.Append(chunk3);
  collection_.Append(other);

  EXPECT_EQ(collection_.GetCount(), 3 * 3);
  EXPECT_EQ(collection_.Chunks().size(), 1);
}

TEST_F(ChunkCollectionTest, AppendCollection) {
  auto chunk1 = CreateChunkWithValue(3);
  auto chunk2 = CreateChunkWithValue(3);
  auto chunk3 = CreateChunkWithValue(3);

  collection_.Append(chunk1);
  ChunkCollection other;
  other.Append(chunk2);
  other.Append(chunk3);

  collection_.Append(other);

  EXPECT_EQ(collection_.GetCount(), 3 * 3);
  EXPECT_EQ(collection_.Chunks().size(), 1);
}

TEST_F(ChunkCollectionTest, MergeOnEmptyCollections) {
  auto chunk = CreateChunkWithValue(2);
  ChunkCollection other;
  other.Append(chunk);
  collection_.Merge(other);

  EXPECT_EQ(collection_.GetCount(), 2);
  EXPECT_EQ(collection_.GetTypes().size(), 3);
  EXPECT_EQ(collection_.GetValue(0, 1), Value(1));
}

TEST_F(ChunkCollectionTest, MergeCollections) {
  auto chunk1 = CreateChunkWithValue(2);
  auto chunk2 = CreateChunkWithValue(2);
  ChunkCollection other;
  other.Append(chunk1);
  collection_.Append(chunk2);

  collection_.Merge(other);

  EXPECT_EQ(collection_.GetCount(), 4);
  EXPECT_EQ(collection_.GetTypes().size(), 3);
  EXPECT_EQ(collection_.GetValue(1, 0), collection_.GetValue(1, 2));
  EXPECT_EQ(collection_.GetValue(1, 1), collection_.GetValue(1, 3));
}

TEST_F(ChunkCollectionTest, FuseCollections) {
  auto chunk1 = CreateChunkWithValue(2);
  auto chunk2 = CreateChunkWithValue(2);
  ChunkCollection other;
  other.Append(chunk1);
  collection_.Append(chunk2);

  collection_.Fuse(other);

  EXPECT_EQ(collection_.ColumnCount(), 6);
  EXPECT_EQ(collection_.GetCount(), 2);
  EXPECT_EQ(collection_.GetValue(0, 1), collection_.GetValue(3, 1));
  EXPECT_EQ(collection_.GetValue(2, 1), collection_.GetValue(5, 1));
}

TEST_F(ChunkCollectionTest, Reset) {
  auto chunk = CreateChunkWithValue(2);
  collection_.Append(chunk);
  collection_.Reset();
  EXPECT_EQ(collection_.GetCount(), 0);
  EXPECT_EQ(collection_.ChunkCount(), 0);
  EXPECT_EQ(collection_.ColumnCount(), 0);
}

TEST_F(ChunkCollectionTest, SetAndGetValue) {
  auto chunk1 = CreateChunkWithValue(STANDARD_VECTOR_SIZE);
  auto chunk2 = CreateChunkWithValue(STANDARD_VECTOR_SIZE);
  auto chunk3 = CreateChunkWithValue(STANDARD_VECTOR_SIZE);
  collection_.Append(chunk1);
  collection_.Append(chunk2);
  collection_.Append(chunk3);
  collection_.SetValue(0, 0, Value(123));
  collection_.SetValue(0, STANDARD_VECTOR_SIZE + 10, Value(123));
  EXPECT_EQ(collection_.GetValue(0, 0), Value(123));
  EXPECT_EQ(collection_.GetValue(0, STANDARD_VECTOR_SIZE + 10), Value(123));
}

TEST_F(ChunkCollectionTest, FetchChunk) {
  auto chunk1 = CreateChunkWithValue(STANDARD_VECTOR_SIZE);
  auto chunk2 = CreateChunkWithValue(STANDARD_VECTOR_SIZE);
  collection_.Append(chunk1);
  collection_.Append(chunk2);
  auto chunk = collection_.Fetch();
  EXPECT_TRUE(chunk != nullptr);
  EXPECT_EQ(collection_.ChunkCount(), 1);
  EXPECT_EQ(collection_.GetCount(), STANDARD_VECTOR_SIZE);
  chunk = collection_.Fetch();
  EXPECT_TRUE(chunk != nullptr);
  EXPECT_EQ(collection_.ChunkCount(), 0);
  EXPECT_EQ(collection_.GetCount(), 0);
}

TEST_F(ChunkCollectionTest, EqualsMethod) {
  auto chunk1 = CreateChunkWithValue(2);
  auto chunk2 = CreateChunkWithValue(2);
  ChunkCollection other;
  collection_.Append(chunk1);
  other.Append(chunk2);
  EXPECT_TRUE(collection_.Equals(other));
  other.SetValue(0, 1, Value(99));
  EXPECT_FALSE(collection_.Equals(other));
}

TEST_F(ChunkCollectionTest, CopyCell) {
  auto chunk1 = CreateChunkWithValue(10);
  collection_.Append(chunk1);
  Vector target(PhysicalType::UINTEGER);
  collection_.CopyCell(1, 5, target, 0);
  EXPECT_EQ(target.GetValue(0), Value(static_cast<uint32_t>(50)));
}

}  // namespace bumblebee
