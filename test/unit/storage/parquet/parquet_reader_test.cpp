//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_reader_test.cpp
//
// Identification: test/unit/storage/parquet/parquet_reader_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_reader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <unordered_set>

#include "parquet_test_base.h"

namespace bumblebee {

/**
 * Scans corpus files (produced by external writers: pandas/pyarrow, various codecs) and compares
 * every cell against the expected CSVs generated from the same files by an external reader.
 */
class ParquetScanTest : public ParquetTestBase {
 protected:
  auto GetFilePath(const std::string &filename) -> std::string override {
    std::filesystem::path test_file_path = __FILE__;
    return (test_file_path.parent_path() / "data" / filename).string();
  }

  auto GetExpectedFilePath(const std::string &filename) -> std::string override {
    std::filesystem::path test_file_path = __FILE__;
    return (test_file_path.parent_path() / "expected" / filename).string();
  }

  void TestParquetReader(const std::string &file, std::vector<std::string> columns = {}) {
    auto filepath = GetFilePath(file);
    ParquetReader reader(allocator_, filepath);
    auto &all_column_names = reader.names_;
    std::vector<idx_t> column_ids;
    std::vector<idx_t> slice_cols;
    if (columns.empty()) {
      for (idx_t i = 0; i < all_column_names.size(); ++i) {
        column_ids.push_back(i);
        columns.push_back(StringUtil::Lower(all_column_names[i]));
        slice_cols.push_back(i);
      }
    } else {
      std::unordered_set<std::string> columns_set(columns.begin(), columns.end());
      for (idx_t i = 0; i < all_column_names.size(); ++i) {
        if (columns_set.contains(StringUtil::Lower(all_column_names[i]))) {
          column_ids.push_back(i);
          slice_cols.push_back(i);
        } else {
          column_ids.push_back(COLUMN_IDENTIFIER_ROW_ID);
        }
      }
    }
    ASSERT_EQ(slice_cols.size(), columns.size());

    auto rows = reader.NumRows();
    std::vector<idx_t> groups_to_read;
    for (idx_t i = 0; i < reader.NumRowGroups(); ++i) {
      groups_to_read.push_back(i);
    }

    ParquetReaderScanState state;
    reader.InitializeScan(state, column_ids, groups_to_read);
    DataChunk chunk;
    chunk.Initialize(reader.return_types_);

    // Load the expected CSV (same filename, but in expected/ and .csv extension).
    const auto expected_csv_path = GetExpectedFilePath(ReplaceExtensionToCsv(file));
    auto expected_all = LoadExpectedCsvColumnMajor(expected_csv_path, columns);

    ASSERT_EQ(static_cast<idx_t>(expected_all.empty() ? 0 : expected_all[0].size()), rows)
        << "Expected CSV row count does not match parquet row count";

    // Scan + compare chunk-by-chunk.
    idx_t offset = 0;

    reader.Scan(state, chunk);
    while (chunk.GetSize() > 0) {
      const idx_t chunk_sz = chunk.GetSize();

      ASSERT_LE(offset + chunk_sz, static_cast<idx_t>(expected_all[0].size())) << "Chunk exceeds expected CSV size";

      auto expected_slice = SliceExpected(expected_all, offset, chunk_sz);
      DataChunk projected;
      projected.InitAndReference(chunk, slice_cols);

      CompareResult(expected_slice, projected);

      offset += chunk_sz;

      chunk.Reset();
      reader.Scan(state, chunk);
    }

    EXPECT_EQ(offset, rows) << "Total scanned rows do not match parquet metadata rows";
  }
};

TEST_F(ParquetScanTest, SimpleParquetScanTest) { TestParquetReader("t1.parquet"); }

TEST_F(ParquetScanTest, SimpleParquetScanOneColTest) {
  TestParquetReader("t1.parquet", {"i"});
  TestParquetReader("t1.parquet", {"j"});
}

TEST_F(ParquetScanTest, LineItemParquetScanTest) { TestParquetReader("lineitem-arrow.parquet"); }

TEST_F(ParquetScanTest, LineItemParquetSelectScanTest) {
  TestParquetReader("lineitem-arrow.parquet", {"l_partkey", "l_suppkey", "l_discount"});
}

TEST_F(ParquetScanTest, GZipParquetScanTest) { TestParquetReader("data_gzip.parquet"); }

TEST_F(ParquetScanTest, SnappyParquetScanTest) { TestParquetReader("data_snappy.parquet"); }

TEST_F(ParquetScanTest, ZSTDParquetScanTest) { TestParquetReader("data_zstd.parquet"); }

TEST_F(ParquetScanTest, Int32DecimalParquetScanTest) { TestParquetReader("int32_decimal.parquet"); }

TEST_F(ParquetScanTest, TimestampParquetScanTest) { TestParquetReader("timestamp.parquet"); }

TEST_F(ParquetScanTest, TimestampMsParquetScanTest) { TestParquetReader("timestamp-ms.parquet"); }

TEST_F(ParquetScanTest, MiniHitsParquetScanTest) { TestParquetReader("mini_hits.parquet"); }

TEST_F(ParquetScanTest, NullParquetScanTest) { TestParquetReader("null.parquet"); }

}  // namespace bumblebee
