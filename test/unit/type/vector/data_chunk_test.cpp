//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// data_chunk_test.cpp
//
// Identification: test/unit/type/vector/data_chunk_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/data_chunk.h"

#include <vector>

#include "gtest/gtest.h"
#include "type/value.h"
#include "type/vector/selection_vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/**
 * @brief Fill `chunk` with synthetic data: cell (i, j) holds `base + i * 100 * (j + 1)`.
 *
 * The value is cast to the type of the column it lands in.
 *
 * @param chunk The chunk to fill. Must already be Initialize()d.
 * @param cardinality The number of rows to write.
 * @param base The value column 0 starts from.
 */
void FillChunk(DataChunk &chunk, unsigned cardinality, int base = 0) {
  chunk.SetCardinality(cardinality);
  for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
    auto ctype = chunk.data_[i].GetLogicalType();
    for (idx_t j = 0; j < chunk.GetSize(); j++) {
      auto numeric_value = base + i * 100 * (j + 1);
      auto v = Value(numeric_value);
      chunk.SetValue(i, j, v.CastAs(ctype));
    }
  }
}

TEST(DataChunkTests, InitializationInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  EXPECT_EQ(chunk.ColumnCount(), 2);
  EXPECT_EQ(chunk.GetSize(), 0);
}

TEST(DataChunkTests, EmptyInitializationInt32) {
  DataChunk chunk;
  chunk.InitializeEmpty(
      std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::BIGINT, PhysicalType::STRING});
  EXPECT_EQ(chunk.ColumnCount(), 3);
  EXPECT_EQ(chunk.GetSize(), 0);
}

TEST(DataChunkTests, SetAndGetValueInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::UTINYINT});
  chunk.SetCardinality(1);
  auto v = Value(static_cast<int32_t>(42));
  chunk.SetValue(0, 0, v);
  EXPECT_EQ(chunk.GetValue(0, 0), v);
  v = Value(static_cast<uint8_t>(10));
  chunk.SetValue(1, 0, v);
  EXPECT_EQ(chunk.GetValue(1, 0), v);
}

TEST(DataChunkTests, ReferenceInt32) {
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::UINTEGER});
  FillChunk(chunk1, 10);

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::UINTEGER});
  chunk2.Reference(chunk1);

  // Updating chunk1 must be reflected in chunk2: they share the data.
  chunk1.SetValue(0, 0, Value(12345));
  for (idx_t i = 0; i < chunk2.ColumnCount(); i++) {
    for (idx_t j = 0; j < chunk2.GetSize(); j++) {
      EXPECT_EQ(chunk2.GetValue(i, j), chunk1.GetValue(i, j));
    }
  }
}

TEST(DataChunkTests, AppendWithoutResizeInt32) {
  auto init_cardinality_chunk1 = 10;
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk1, init_cardinality_chunk1);

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk2, 10);

  chunk1.Append(chunk2);
  EXPECT_EQ(chunk1.GetSize(), 20);
  for (idx_t i = 0; i < chunk2.ColumnCount(); i++) {
    for (idx_t j = 0; j < chunk2.GetSize(); j++) {
      EXPECT_EQ(chunk2.GetValue(i, j), chunk1.GetValue(i, j + init_cardinality_chunk1));
    }
  }
}

TEST(DataChunkTests, AppendWithSelectionVectorInt32) {
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::UBIGINT});
  FillChunk(chunk1, 10);

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::UBIGINT});
  FillChunk(chunk2, 20);

  // The last 10 rows of chunk2: 0 -> 10, 1 -> 11, ..., 9 -> 19.
  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);
  }

  chunk1.Append(chunk2, true, &sel, 10);
  EXPECT_EQ(chunk1.GetSize(), 20);
  for (idx_t i = 0; i < chunk2.ColumnCount(); i++) {
    for (idx_t j = 10; j < chunk2.GetSize(); j++) {
      EXPECT_EQ(chunk2.GetValue(i, j), chunk1.GetValue(i, j));
    }
  }
}

TEST(DataChunkTests, CopyDataChunkInt32) {
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk1, 100);

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk2, 100, 1000);
  // The two chunks start out different.
  for (idx_t i = 0; i < chunk2.ColumnCount(); i++) {
    for (idx_t j = 0; j < chunk2.GetSize(); j++) {
      EXPECT_NE(chunk2.GetValue(i, j), chunk1.GetValue(i, j));
    }
  }

  chunk1.SetCardinality(0);
  chunk2.Copy(chunk1);
  EXPECT_EQ(chunk2.GetSize(), 100);
  for (idx_t i = 0; i < chunk2.ColumnCount(); i++) {
    for (idx_t j = 0; j < chunk2.GetSize(); j++) {
      EXPECT_EQ(chunk2.GetValue(i, j), chunk1.GetValue(i, j));
    }
  }
}

TEST(DataChunkTests, SliceAndNormalifyInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk, 20);

  // The last 10 rows: 0 -> 10, 1 -> 11, ..., 9 -> 19.
  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);
  }

  chunk.Slice(sel, 10);
  chunk.Normalify();
  EXPECT_EQ(chunk.GetSize(), 10);
  // Column 0 is all zeros.
  for (idx_t j = 0; j < chunk.GetSize(); j++) {
    EXPECT_EQ(chunk.GetValue(0, j), Value(0));
  }
  // Column 1 is [1100, 1200, ..., 2000].
  for (idx_t j = 0; j < chunk.GetSize(); j++) {
    auto val_expected = 1000 + (j + 1) * 100;
    EXPECT_EQ(chunk.GetValue(1, j), Value(static_cast<int32_t>(val_expected)));
  }
}

TEST(DataChunkTests, ResetInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::BIGINT, PhysicalType::BIGINT});
  FillChunk(chunk, 1000);
  chunk.Reset();
  EXPECT_EQ(chunk.GetSize(), 0);
  EXPECT_EQ(chunk.ColumnCount(), 2);
}

TEST(DataChunkTests, SplitInt32) {
  DataChunk chunk1;
  chunk1.Initialize(
      std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER, PhysicalType::INTEGER});

  DataChunk original_chunk;
  original_chunk.Initialize(
      std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER, PhysicalType::INTEGER});
  original_chunk.SetCardinality(100);
  FillChunk(original_chunk, 100);
  original_chunk.Copy(chunk1);

  DataChunk chunk2;
  // Columns 0 and 1 stay in chunk1; column 2 moves to chunk2.
  chunk1.Split(chunk2, 2);
  EXPECT_EQ(chunk1.ColumnCount(), 2);
  EXPECT_EQ(chunk2.ColumnCount(), 1);
  EXPECT_EQ(chunk2.GetSize(), original_chunk.GetSize());
  EXPECT_EQ(chunk1.GetSize(), original_chunk.GetSize());
  for (idx_t j = 0; j < original_chunk.GetSize(); j++) {
    EXPECT_EQ(original_chunk.GetValue(0, j), chunk1.GetValue(0, j));
    EXPECT_EQ(original_chunk.GetValue(1, j), chunk1.GetValue(1, j));
  }
  for (idx_t j = 0; j < original_chunk.GetSize(); j++) {
    EXPECT_EQ(original_chunk.GetValue(2, j), chunk2.GetValue(0, j));
  }
}

TEST(DataChunkTests, HashInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::UINTEGER});
  FillChunk(chunk, 50);

  Vector hash_vec1(LogicalTypeId::HASH);
  chunk.Hash(hash_vec1);
  EXPECT_EQ(hash_vec1.GetType(), PhysicalType::UBIGINT);
  EXPECT_EQ(hash_vec1.GetVectorType(), VectorType::FLAT_VECTOR);

  Vector hash_vec2(LogicalTypeId::HASH);
  chunk.Hash(hash_vec2);
  EXPECT_EQ(hash_vec2.GetType(), PhysicalType::UBIGINT);
  EXPECT_EQ(hash_vec2.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < chunk.GetSize(); i++) {
    EXPECT_EQ(hash_vec1.GetValue(i), hash_vec2.GetValue(i));
  }
}

TEST(DataChunkTests, OrrifyInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk, 20);

  // The last 10 rows: 0 -> 10, 1 -> 11, ..., 9 -> 19.
  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);
  }
  chunk.Slice(sel, 10);

  auto data = chunk.Orrify();
  EXPECT_NE(data, nullptr);
  for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
    const auto *col_data = reinterpret_cast<const int32_t *>(data[i].data_);
    const auto *sel_p = data[i].sel_;
    for (idx_t j = 0; j < chunk.GetSize(); j++) {
      EXPECT_EQ(col_data[sel_p->GetIndex(j)], chunk.GetValue(i, j).GetAs<int32_t>());
    }
  }
}

TEST(DataChunkTests, OrrifyWithSelectionInt32) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk, 10);

  auto data = chunk.Orrify();
  EXPECT_NE(data, nullptr);
  for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
    const auto *col_data = reinterpret_cast<const int32_t *>(data[i].data_);
    for (idx_t j = 0; j < chunk.GetSize(); j++) {
      EXPECT_EQ(col_data[j], chunk.GetValue(i, j).GetAs<int32_t>());
    }
  }
}

// ---------- Multi-type chunks ----------
//
// The INTEGER-only suite above cannot catch a per-type bug in the column movement — most of
// all for a STRING column, whose values live in a shared heap rather than inline. These mirror
// the movement ops over a mixed INTEGER + STRING + DOUBLE chunk. FillChunk casts its synthetic
// numeric value into each column's own type, so every column carries type-correct data.
// (DECIMAL / LIST are omitted here because Value::CastAs does not target them; the LIST column
// path is covered by list_vector_test's DataChunkAppendWithAListColumn.)

static auto MixedTypes() -> std::vector<LogicalType> {
  return {LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::STRING), LogicalType(LogicalTypeId::DOUBLE)};
}

TEST(DataChunkMixedTests, AppendMixedTypes) {
  DataChunk chunk1;
  chunk1.Initialize(MixedTypes());
  FillChunk(chunk1, 10);

  DataChunk chunk2;
  chunk2.Initialize(MixedTypes());
  FillChunk(chunk2, 10, 5000);

  chunk1.Append(chunk2);
  EXPECT_EQ(chunk1.GetSize(), 20);
  for (idx_t i = 0; i < chunk2.ColumnCount(); i++) {
    for (idx_t j = 0; j < chunk2.GetSize(); j++) {
      EXPECT_EQ(chunk2.GetValue(i, j), chunk1.GetValue(i, j + 10)) << "col " << i << " row " << j;
    }
  }
}

TEST(DataChunkMixedTests, CopyMixedTypes) {
  DataChunk src;
  src.Initialize(MixedTypes());
  FillChunk(src, 50);

  DataChunk dst;
  dst.Initialize(MixedTypes());
  dst.SetCardinality(0);
  src.Copy(dst);
  EXPECT_EQ(dst.GetSize(), 50);
  for (idx_t i = 0; i < src.ColumnCount(); i++) {
    for (idx_t j = 0; j < src.GetSize(); j++) {
      EXPECT_EQ(src.GetValue(i, j), dst.GetValue(i, j)) << "col " << i << " row " << j;
    }
  }
}

TEST(DataChunkMixedTests, SliceMixedTypes) {
  DataChunk chunk;
  chunk.Initialize(MixedTypes());
  FillChunk(chunk, 20);

  // Snapshot the last 10 rows before slicing, since Slice rewrites the chunk in place.
  std::vector<std::vector<Value>> expected(chunk.ColumnCount());
  for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
    for (idx_t j = 10; j < 20; j++) {
      expected[i].push_back(chunk.GetValue(i, j));
    }
  }

  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);
  }
  chunk.Slice(sel, 10);
  chunk.Normalify();
  EXPECT_EQ(chunk.GetSize(), 10);
  for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
    for (idx_t j = 0; j < 10; j++) {
      EXPECT_EQ(chunk.GetValue(i, j), expected[i][j]) << "col " << i << " row " << j;
    }
  }
}

TEST(DataChunkMixedTests, SplitMixedTypes) {
  DataChunk chunk;
  chunk.Initialize(MixedTypes());
  FillChunk(chunk, 30);

  // Snapshot the DOUBLE column (index 2) before it moves out.
  std::vector<Value> col2;
  for (idx_t j = 0; j < 30; j++) {
    col2.push_back(chunk.GetValue(2, j));
  }

  DataChunk tail;
  chunk.Split(tail, 2);  // columns 0,1 stay; column 2 (DOUBLE) moves to tail
  EXPECT_EQ(chunk.ColumnCount(), 2);
  EXPECT_EQ(tail.ColumnCount(), 1);
  EXPECT_EQ(tail.data_[0].GetLogicalTypeId(), LogicalTypeId::DOUBLE);
  for (idx_t j = 0; j < 30; j++) {
    EXPECT_EQ(tail.GetValue(0, j), col2[j]) << "row " << j;
  }
}

// ---------- Reference / Clone / InitAndReference / cols_map slice ----------

TEST(DataChunkReferenceTests, CloneSharesData) {
  DataChunk chunk;
  chunk.Initialize(MixedTypes());
  FillChunk(chunk, 10);

  auto clone = chunk.Clone();
  EXPECT_EQ(clone->ColumnCount(), 3);
  EXPECT_EQ(clone->GetSize(), 10);
  // Clone references the source, so a source write shows through.
  chunk.SetValue(0, 0, Value(999));
  EXPECT_EQ(clone->GetValue(0, 0), chunk.GetValue(0, 0));
}

TEST(DataChunkReferenceTests, InitAndReferenceAllColumns) {
  DataChunk chunk;
  chunk.Initialize(MixedTypes());
  FillChunk(chunk, 8);

  DataChunk ref;
  ref.InitAndReference(chunk);
  EXPECT_EQ(ref.ColumnCount(), 3);
  for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
    for (idx_t j = 0; j < 8; j++) {
      EXPECT_EQ(ref.GetValue(i, j), chunk.GetValue(i, j)) << "col " << i << " row " << j;
    }
  }
}

TEST(DataChunkReferenceTests, InitAndReferenceSubsetColumns) {
  DataChunk chunk;
  chunk.Initialize(MixedTypes());
  FillChunk(chunk, 8);

  DataChunk ref;
  ref.InitAndReference(chunk, std::vector<idx_t>{0, 1});  // drop the DECIMAL column
  EXPECT_EQ(ref.ColumnCount(), 2);
  for (idx_t j = 0; j < 8; j++) {
    EXPECT_EQ(ref.GetValue(0, j), chunk.GetValue(0, j));
    EXPECT_EQ(ref.GetValue(1, j), chunk.GetValue(1, j));
  }
}

TEST(DataChunkReferenceTests, SliceWithColsMapRemapsColumns) {
  DataChunk other;
  other.Initialize(MixedTypes());
  FillChunk(other, 20);

  // Target with three columns; the map routes other's column c into target column {2,1,0}.
  DataChunk target;
  target.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER, PhysicalType::INTEGER});

  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);
  }
  target.Slice(other, sel, 10, std::vector<idx_t>{2, 1, 0});
  EXPECT_EQ(target.GetSize(), 10);
  // other col 0 (INTEGER) landed in target col 2.
  for (idx_t j = 0; j < 10; j++) {
    EXPECT_EQ(target.GetValue(2, j), other.GetValue(0, j + 10)) << "row " << j;
    EXPECT_EQ(target.GetValue(0, j), other.GetValue(2, j + 10)) << "row " << j;  // DOUBLE -> col 0
  }
}

TEST(DataChunkReferenceTests, PartialResetPreservesColumnCount) {
  DataChunk chunk;
  chunk.Initialize(MixedTypes());
  FillChunk(chunk, 100);

  // Partial reset re-initializes ONLY the named columns with backing storage; the others are
  // left with null data (the chunk is meant to reference them), so only 0 and 2 are writable.
  chunk.Reset(std::vector<idx_t>{0, 2});
  EXPECT_EQ(chunk.GetSize(), 0);
  EXPECT_EQ(chunk.ColumnCount(), 3);

  chunk.SetCardinality(5);
  for (idx_t j = 0; j < 5; j++) {
    chunk.SetValue(0, j, Value(static_cast<int32_t>(j)));
    chunk.SetValue(2, j, Value(static_cast<double>(j) * 1.5));
  }
  for (idx_t j = 0; j < 5; j++) {
    EXPECT_EQ(chunk.GetValue(0, j), Value(static_cast<int32_t>(j)));
    EXPECT_EQ(chunk.GetValue(2, j), Value(static_cast<double>(j) * 1.5));
  }
}

// ---------- NULL propagation across DataChunk movement ----------

TEST(DataChunkNullTests, AppendCarriesNull) {
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk1, 10);

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk2, 10);
  chunk2.SetValue(1, 4, Value::Null());  // null in col 1, row 4

  chunk1.Append(chunk2);
  EXPECT_EQ(chunk1.GetSize(), 20);
  // The null landed at row 10 + 4 in chunk1.
  EXPECT_TRUE(chunk1.GetValue(1, 14).IsNull());
  EXPECT_FALSE(chunk1.GetValue(1, 13).IsNull());
}

TEST(DataChunkNullTests, AppendWithSelectionCarriesNull) {
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk1, 10);

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk2, 20);
  chunk2.SetValue(0, 15, Value::Null());  // selected below

  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);  // picks rows 10..19
  }

  chunk1.Append(chunk2, true, &sel, 10);
  EXPECT_EQ(chunk1.GetSize(), 20);
  // Source row 15 maps to sel position 5 -> target row 10 + 5 = 15.
  EXPECT_TRUE(chunk1.GetValue(0, 15).IsNull());
  EXPECT_FALSE(chunk1.GetValue(0, 14).IsNull());
}

TEST(DataChunkNullTests, CopyCarriesNull) {
  DataChunk src;
  src.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(src, 50);
  src.SetValue(0, 7, Value::Null());

  DataChunk dst;
  dst.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  dst.SetCardinality(0);
  src.Copy(dst);
  EXPECT_EQ(dst.GetSize(), 50);
  EXPECT_TRUE(dst.GetValue(0, 7).IsNull());
  EXPECT_FALSE(dst.GetValue(0, 6).IsNull());
}

TEST(DataChunkNullTests, SliceReadsNull) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk, 20);
  chunk.SetValue(1, 13, Value::Null());

  SelectionVector sel(10);
  for (idx_t i = 0; i < 10; i++) {
    sel.SetIndex(i, i + 10);  // rows 10..19
  }

  chunk.Slice(sel, 10);
  // Source row 13 -> sliced position 3.
  EXPECT_TRUE(chunk.GetValue(1, 3).IsNull());
  EXPECT_FALSE(chunk.GetValue(1, 2).IsNull());
}

TEST(DataChunkNullTests, ResetClearsNull) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::BIGINT, PhysicalType::BIGINT});
  FillChunk(chunk, 100);
  chunk.SetValue(0, 3, Value::Null());
  EXPECT_TRUE(chunk.GetValue(0, 3).IsNull());

  chunk.Reset();
  FillChunk(chunk, 100);
  // After reset + refill the previously-null slot is a normal value again.
  EXPECT_FALSE(chunk.GetValue(0, 3).IsNull());
}

TEST(DataChunkNullTests, CastCarriesNull) {
  DataChunk chunk;
  chunk.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk, 30);
  chunk.SetValue(0, 9, Value::Null());

  chunk.Cast(std::vector<LogicalType>{PhysicalType::BIGINT, PhysicalType::INTEGER});
  EXPECT_EQ(chunk.data_[0].GetType(), PhysicalType::BIGINT);
  EXPECT_TRUE(chunk.GetValue(0, 9).IsNull());
  EXPECT_FALSE(chunk.GetValue(0, 8).IsNull());
}

TEST(DataChunkNullTests, CastIntoResultCarriesNull) {
  DataChunk src;
  src.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(src, 30);
  src.SetValue(0, 9, Value::Null());

  DataChunk result;
  result.Initialize(std::vector<PhysicalType>{PhysicalType::BIGINT, PhysicalType::INTEGER});
  result.SetCardinality(30);
  src.Cast(result);
  EXPECT_TRUE(result.GetValue(0, 9).IsNull());
  EXPECT_FALSE(result.GetValue(0, 8).IsNull());
}

TEST(DataChunkNullTests, CopyWithSelectionCarriesNull) {
  DataChunk src;
  src.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(src, 20);
  src.SetValue(1, 7, Value::Null());

  SelectionVector sel(20);
  for (idx_t i = 0; i < 20; i++) {
    sel.SetIndex(i, i);
  }

  DataChunk dst;
  dst.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  src.Copy(dst, sel, 20, 0);
  EXPECT_EQ(dst.GetSize(), 20);
  EXPECT_TRUE(dst.GetValue(1, 7).IsNull());
  EXPECT_FALSE(dst.GetValue(1, 6).IsNull());
}

TEST(DataChunkNullTests, AppendWithResizePreservesNull) {
  DataChunk chunk1;
  chunk1.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk1, 1000);
  chunk1.SetValue(0, 5, Value::Null());

  DataChunk chunk2;
  chunk2.Initialize(std::vector<PhysicalType>{PhysicalType::INTEGER, PhysicalType::INTEGER});
  FillChunk(chunk2, 300);
  chunk2.SetValue(0, 1, Value::Null());

  chunk1.Append(chunk2, true);  // 1000 + 300 = 1300 > capacity 1024 -> resize
  EXPECT_EQ(chunk1.GetSize(), 1300);
  EXPECT_TRUE(chunk1.GetValue(0, 5).IsNull());         // the target's null survived the resize
  EXPECT_TRUE(chunk1.GetValue(0, 1000 + 1).IsNull());  // the appended null landed
  EXPECT_FALSE(chunk1.GetValue(0, 999).IsNull());
}

}  // namespace bumblebee
