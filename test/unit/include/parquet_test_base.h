//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_test_base.h
//
// Identification: test/unit/include/parquet_test_base.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/util/string_util.h"
#include "storage/parquet/parquet_reader.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/**
 * @brief Base fixture for parquet tests: loads expected CSVs (produced from the same files by an
 * external reader, see generate_expected.sh) and compares them cell-by-cell against scanned
 * chunks rendered as strings.
 */
class ParquetTestBase : public ::testing::Test {
 protected:
  Allocator allocator_;

  virtual auto GetFilePath(const std::string &filename) -> std::string = 0;
  virtual auto GetExpectedFilePath(const std::string &filename) -> std::string = 0;

  static auto ReplaceExtensionToCsv(const std::string &parquet_filename) -> std::string {
    std::filesystem::path p(parquet_filename);
    p.replace_extension(".csv");
    return p.string();
  }

  /** @brief Minimal CSV row parser (handles quoted fields and escaped quotes ""). */
  static auto ParseCsvRow(const std::string &line, char delimiter = ',') -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); i++) {
      char c = line[i];

      if (in_quotes) {
        if (c == '"') {
          if (i + 1 < line.size() && line[i + 1] == '"') {
            cur.push_back('"');
            i++;
          } else {
            in_quotes = false;
          }
        } else {
          cur.push_back(c);
        }
      } else {
        if (c == '"') {
          in_quotes = true;
        } else if (c == delimiter) {
          out.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
    }
    out.push_back(cur);
    return out;
  }

  /** @brief Loads a CSV into column-major storage: expected[col][row], selecting `columns` by
   * (case-normalized) header name. */
  static auto LoadExpectedCsvColumnMajor(const std::string &csv_path, const std::vector<std::string> &columns,
                                         char delimiter = ',') -> std::vector<std::vector<std::string>> {
    std::ifstream in(csv_path);
    EXPECT_TRUE(in.is_open()) << "Could not open expected CSV: " << csv_path;

    std::vector<std::vector<std::string>> cols;
    cols.resize(columns.size());

    std::string line;
    std::getline(in, line);
    auto header = ParseCsvRow(line, delimiter);
    std::unordered_map<std::string, size_t> header_index;
    for (size_t i = 0; i < header.size(); i++) {
      header_index.emplace(StringUtil::Lower(header[i]), i);
    }
    std::vector<size_t> col_indices;
    col_indices.reserve(columns.size());

    for (const auto &col_name : columns) {
      auto it = header_index.find(StringUtil::Lower(col_name));
      EXPECT_TRUE(it != header_index.end()) << "column " << col_name << " not in expected CSV header";
      col_indices.push_back(it->second);
    }

    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      auto fields = ParseCsvRow(line, delimiter);
      for (size_t c = 0; c < col_indices.size(); c++) {
        size_t idx = col_indices[c];
        cols[c].push_back(idx < fields.size() ? fields[idx] : "");
      }
    }

    return cols;
  }

  /** @brief Slice column-major expected data for a given [offset, offset+count). */
  static auto SliceExpected(const std::vector<std::vector<std::string>> &expected_all, idx_t offset, idx_t count)
      -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> sliced;
    sliced.resize(expected_all.size());
    for (size_t c = 0; c < expected_all.size(); c++) {
      auto begin = expected_all[c].begin() + static_cast<std::ptrdiff_t>(offset);
      auto end = begin + static_cast<std::ptrdiff_t>(count);
      sliced[c].assign(begin, end);
    }
    return sliced;
  }

  /** @brief Compare a chunk (cast to strings) against expected column-major cells. */
  static void CompareResult(std::vector<std::vector<std::string>> &expected, DataChunk &chunk) {
    EXPECT_EQ(chunk.ColumnCount(), expected.size());
    std::vector<LogicalType> string_types{chunk.ColumnCount(), LogicalType(LogicalTypeId::STRING)};
    DataChunk string_chunk;
    string_chunk.InitAndReference(chunk);
    string_chunk.Cast(string_types);

    for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
      EXPECT_EQ(expected[col].size(), string_chunk.GetSize());
      for (idx_t row = 0; row < string_chunk.GetSize(); row++) {
        Value val = string_chunk.GetValue(col, row);
        std::string str = val.IsNull() ? "NULL" : val.ToString();
        // ToString() single-quotes string values; the CSV holds the bare rendering.
        if (str.size() >= 2 && str.front() == '\'' && str.back() == '\'') {
          str = str.substr(1, str.length() - 2);
        }
        EXPECT_EQ(expected[col][row], str) << "col " << col << " row " << row;
      }
    }
  }
};

}  // namespace bumblebee
