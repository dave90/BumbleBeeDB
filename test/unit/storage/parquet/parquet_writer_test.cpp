//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_writer_test.cpp
//
// Identification: test/unit/storage/parquet/parquet_writer_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_writer.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "storage/parquet/parquet_reader.h"
#include "type/value.h"
#include "type/vector/chunk_collection.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/**
 * Round-trips chunks through ParquetWriter -> ParquetReader, covering every writable type, NULL
 * scattering (real definition levels + skipped payload entries), multi-chunk row groups spanning
 * STANDARD_VECTOR_SIZE, and all compression codecs.
 */
class ParquetWriterTest : public ::testing::Test {
 protected:
  Allocator allocator_;

  auto GetOutputPath(const std::string &name) -> std::string {
    auto dir = std::filesystem::temp_directory_path() / "bumblebee_parquet_test";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
  }

  /** @brief Read the whole file back into one flat list of chunks. */
  auto ReadFile(const std::string &path, std::vector<LogicalType> &out_types) -> std::vector<data_chunk_ptr_t> {
    ParquetReader reader(allocator_, path);
    out_types = reader.return_types_;

    std::vector<idx_t> column_ids;
    for (idx_t i = 0; i < reader.names_.size(); i++) {
      column_ids.push_back(i);
    }
    std::vector<idx_t> groups;
    for (idx_t i = 0; i < reader.NumRowGroups(); i++) {
      groups.push_back(i);
    }

    ParquetReaderScanState state;
    reader.InitializeScan(state, column_ids, groups);

    std::vector<data_chunk_ptr_t> chunks;
    DataChunk chunk;
    chunk.Initialize(reader.return_types_);
    reader.Scan(state, chunk);
    while (chunk.GetSize() > 0) {
      chunks.push_back(chunk.Clone());
      chunk.Reset();
      reader.Scan(state, chunk);
    }
    return chunks;
  }
};

// INTEGER / STRING / DOUBLE columns with NULLs scattered across rows and columns survive a
// write->read round-trip: NULL cells read back NULL, the rest read back their exact value.
TEST_F(ParquetWriterTest, MixedNullsRoundTrip) {
  auto out_path = GetOutputPath("out_null_mixed.parquet");

  const idx_t n = 7;
  std::vector<LogicalType> types = {LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::STRING),
                                    LogicalType(LogicalTypeId::DOUBLE)};
  std::vector<std::string> names = {"A", "B", "C"};
  DataChunk chunk;
  chunk.Initialize(types);
  chunk.SetCardinality(n);
  for (idx_t i = 0; i < n; i++) {
    chunk.SetValue(0, i, Value(static_cast<int32_t>(i * 10)));
    chunk.SetValue(1, i, Value("s" + std::to_string(i)));
    chunk.SetValue(2, i, Value(static_cast<double>(i) + 0.5));
  }
  // NULLs: col A rows {1,4}, col B rows {0,4}, col C rows {4,6}. Row 4 all-null.
  chunk.SetValue(0, 1, Value::Null());
  chunk.SetValue(0, 4, Value::Null());
  chunk.SetValue(1, 0, Value::Null());
  chunk.SetValue(1, 4, Value::Null());
  chunk.SetValue(2, 4, Value::Null());
  chunk.SetValue(2, 6, Value::Null());

  ChunkCollection collection;
  collection.Append(chunk);

  ParquetWriter writer(out_path, types, names, format::CompressionCodec::UNCOMPRESSED);
  writer.Flush(collection);
  writer.Finalize();

  std::vector<LogicalType> rtypes;
  auto out = ReadFile(out_path, rtypes);

  ASSERT_EQ(out.size(), 1u);
  auto &c = *out[0];
  ASSERT_EQ(c.GetSize(), n);

  auto expect_null = [&](idx_t col, idx_t row) {
    EXPECT_TRUE(c.GetValue(col, row).IsNull()) << "col " << col << " row " << row;
  };

  expect_null(0, 1);
  expect_null(0, 4);
  expect_null(1, 0);
  expect_null(1, 4);
  expect_null(2, 4);
  expect_null(2, 6);

  EXPECT_EQ(c.GetValue(0, 0).GetAs<int32_t>(), 0);
  EXPECT_EQ(c.GetValue(0, 2).GetAs<int32_t>(), 20);
  EXPECT_FALSE(c.GetValue(1, 1).IsNull());
  EXPECT_EQ(c.GetValue(2, 0).GetAs<double>(), 0.5);
}

// >5000 rows so it spans the STANDARD_VECTOR_SIZE chunk boundary; every 17th row is NULL in both
// columns. Verifies definition-level packing across chunks within one row group.
TEST_F(ParquetWriterTest, MultiBatchNullsRoundTrip) {
  auto out_path = GetOutputPath("out_null_multi.parquet");

  const idx_t n = 5300;
  std::vector<LogicalType> types = {LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::STRING)};
  std::vector<std::string> names = {"K", "V"};

  ChunkCollection collection;
  idx_t produced = 0;
  while (produced < n) {
    idx_t batch = MinValue<idx_t>(STANDARD_VECTOR_SIZE, n - produced);
    DataChunk c;
    c.Initialize(types);
    c.SetCardinality(batch);
    for (idx_t r = 0; r < batch; r++) {
      idx_t i = produced + r;
      if (i % 17 == 0) {
        c.SetValue(0, r, Value::Null());
        c.SetValue(1, r, Value::Null());
      } else {
        c.SetValue(0, r, Value(static_cast<int64_t>(i)));
        c.SetValue(1, r, Value("v" + std::to_string(i)));
      }
    }
    collection.Append(c);
    produced += batch;
  }

  ParquetWriter writer(out_path, types, names, format::CompressionCodec::SNAPPY);
  writer.Flush(collection);
  writer.Finalize();

  std::vector<LogicalType> rtypes;
  auto out = ReadFile(out_path, rtypes);

  idx_t row = 0;
  for (auto &c : out) {
    for (idx_t r = 0; r < c->GetSize(); r++, row++) {
      bool should_null = (row % 17 == 0);
      EXPECT_EQ(c->GetValue(0, r).IsNull(), should_null) << "K row " << row;
      EXPECT_EQ(c->GetValue(1, r).IsNull(), should_null) << "V row " << row;
      if (!should_null) {
        EXPECT_EQ(c->GetValue(0, r).GetAs<int64_t>(), static_cast<int64_t>(row)) << "K row " << row;
        EXPECT_EQ(c->GetValue(1, r).ToString(), "'v" + std::to_string(row) + "'") << "V row " << row;
      }
    }
  }
  EXPECT_EQ(row, n);
}

// Every writable type round-trips through every codec.
TEST_F(ParquetWriterTest, AllTypesAllCodecsRoundTrip) {
  const idx_t n = 50;
  std::vector<LogicalType> types = {
      LogicalType(LogicalTypeId::BOOLEAN), LogicalType(LogicalTypeId::TINYINT),
      LogicalType(LogicalTypeId::SMALLINT), LogicalType(LogicalTypeId::INTEGER),
      LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::FLOAT),
      LogicalType(LogicalTypeId::DOUBLE),  LogicalType(LogicalTypeId::STRING),
  };
  std::vector<std::string> names = {"b", "t", "s", "i", "bi", "f", "d", "str"};

  DataChunk chunk;
  chunk.Initialize(types);
  chunk.SetCardinality(n);
  for (idx_t r = 0; r < n; r++) {
    chunk.SetValue(0, r, Value(r % 2 == 0));
    chunk.SetValue(1, r, Value(static_cast<int8_t>(r % 100)));
    chunk.SetValue(2, r, Value(static_cast<int16_t>(r * 3)));
    chunk.SetValue(3, r, Value(static_cast<int32_t>(r * 1000)));
    chunk.SetValue(4, r, Value(static_cast<int64_t>(r) * int64_t{100000000}));
    chunk.SetValue(5, r, Value(static_cast<float>(r) * 1.5F));
    chunk.SetValue(6, r, Value(static_cast<double>(r) * 2.25));
    chunk.SetValue(7, r, Value("row-" + std::to_string(r)));
  }

  for (auto codec : {format::CompressionCodec::UNCOMPRESSED, format::CompressionCodec::SNAPPY,
                     format::CompressionCodec::GZIP, format::CompressionCodec::ZSTD}) {
    auto out_path = GetOutputPath("out_all_types_" + std::to_string(static_cast<int>(codec)) + ".parquet");

    ChunkCollection collection;
    collection.Append(chunk);
    ParquetWriter writer(out_path, types, names, codec);
    writer.Flush(collection);
    writer.Finalize();

    std::vector<LogicalType> rtypes;
    auto out = ReadFile(out_path, rtypes);
    ASSERT_EQ(out.size(), 1u) << "codec " << static_cast<int>(codec);
    auto &c = *out[0];
    ASSERT_EQ(c.GetSize(), n);

    for (idx_t r = 0; r < n; r++) {
      EXPECT_EQ(c.GetValue(0, r).GetAs<bool>(), r % 2 == 0);
      EXPECT_EQ(c.GetValue(1, r).GetAs<int32_t>(), static_cast<int32_t>(r % 100));
      EXPECT_EQ(c.GetValue(2, r).GetAs<int32_t>(), static_cast<int32_t>(r * 3));
      EXPECT_EQ(c.GetValue(3, r).GetAs<int32_t>(), static_cast<int32_t>(r * 1000));
      EXPECT_EQ(c.GetValue(4, r).GetAs<int64_t>(), static_cast<int64_t>(r) * 100000000LL);
      EXPECT_EQ(c.GetValue(5, r).GetAs<float>(), static_cast<float>(r) * 1.5F);
      EXPECT_EQ(c.GetValue(6, r).GetAs<double>(), static_cast<double>(r) * 2.25);
      EXPECT_EQ(c.GetValue(7, r).ToString(), "'row-" + std::to_string(r) + "'");
    }
  }
}

// Each Flush() emits one row group; the reader concatenates them in order.
TEST_F(ParquetWriterTest, MultipleRowGroups) {
  auto out_path = GetOutputPath("out_row_groups.parquet");

  std::vector<LogicalType> types = {LogicalType(LogicalTypeId::INTEGER)};
  std::vector<std::string> names = {"x"};

  ParquetWriter writer(out_path, types, names, format::CompressionCodec::UNCOMPRESSED);

  const idx_t groups = 3;
  const idx_t rows_per_group = 10;
  for (idx_t g = 0; g < groups; g++) {
    DataChunk chunk;
    chunk.Initialize(types);
    chunk.SetCardinality(rows_per_group);
    for (idx_t r = 0; r < rows_per_group; r++) {
      chunk.SetValue(0, r, Value(static_cast<int32_t>(g * rows_per_group + r)));
    }
    ChunkCollection collection;
    collection.Append(chunk);
    writer.Flush(collection);
  }
  writer.Finalize();

  ParquetReader reader(allocator_, out_path);
  EXPECT_EQ(reader.NumRowGroups(), groups);
  EXPECT_EQ(reader.NumRows(), groups * rows_per_group);

  std::vector<LogicalType> rtypes;
  auto out = ReadFile(out_path, rtypes);
  idx_t row = 0;
  for (auto &c : out) {
    for (idx_t r = 0; r < c->GetSize(); r++, row++) {
      EXPECT_EQ(c->GetValue(0, r).GetAs<int32_t>(), static_cast<int32_t>(row));
    }
  }
  EXPECT_EQ(row, groups * rows_per_group);
}

}  // namespace bumblebee
