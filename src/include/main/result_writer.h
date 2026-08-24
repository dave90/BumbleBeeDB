//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// result_writer.h
//
// Identification: src/include/main/result_writer.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"

namespace bumblebee {

class QueryResult;

/** Where a typed native result is rendered for a shell or test frontend. */
class ResultWriter {
 public:
  ResultWriter() = default;
  virtual ~ResultWriter() = default;

  virtual void WriteCell(const std::string &cell) = 0;
  virtual void WriteHeaderCell(const std::string &cell) = 0;
  virtual void BeginHeader() = 0;
  virtual void EndHeader() = 0;
  virtual void BeginRow() = 0;
  virtual void EndRow() = 0;
  virtual void BeginTable(bool simplified_output) = 0;
  virtual void EndTable() = 0;

  /** @brief Write a single value as a whole one-cell table. */
  virtual void OneCell(const std::string &cell) {
    BeginTable(true);
    BeginRow();
    WriteCell(cell);
    EndRow();
    EndTable();
  }

  /** @return The most rows this writer wants emitted; zero means unlimited. */
  virtual auto MaxDisplayRows() const -> idx_t { return 0; }

  /** @brief Called once when output was capped. Default no-op. */
  virtual void WriteTruncationNotice(idx_t shown, idx_t total) {}

  bool simplified_output_{false};
};

/** @brief Discards everything. */
class NoopWriter : public ResultWriter {
 public:
  void WriteCell(const std::string &cell) override {}
  void WriteHeaderCell(const std::string &cell) override {}
  void BeginHeader() override {}
  void EndHeader() override {}
  void BeginRow() override {}
  void EndRow() override {}
  void BeginTable(bool simplified_output) override {}
  void EndTable() override {}
};

/** @brief Writes separator-delimited plain text to a stream. */
class SimpleStreamWriter : public ResultWriter {
 public:
  explicit SimpleStreamWriter(std::ostream &stream, bool disable_header = false, std::string separator = "\t",
                              idx_t max_display_rows = 0)
      : disable_header_(disable_header),
        stream_(stream),
        separator_(std::move(separator)),
        max_display_rows_(max_display_rows) {}

  void WriteCell(const std::string &cell) override { stream_ << cell << separator_; }
  void WriteHeaderCell(const std::string &cell) override {
    if (!disable_header_) {
      stream_ << cell << separator_;
    }
  }
  void BeginHeader() override {}
  void EndHeader() override {
    if (!disable_header_) {
      stream_ << std::endl;
    }
  }
  void BeginRow() override {}
  void EndRow() override { stream_ << std::endl; }
  void BeginTable(bool simplified_output) override {}
  void EndTable() override {}
  auto MaxDisplayRows() const -> idx_t override { return max_display_rows_; }
  void WriteTruncationNotice(idx_t shown, idx_t total) override {
    stream_ << "-- showing first " << shown << " of " << total << " rows (--max-rows 0 to show all) --" << std::endl;
  }

  bool disable_header_;
  std::ostream &stream_;
  std::string separator_;
  idx_t max_display_rows_;
};

/** @brief Collects rendered cells into rows for native tests. */
class StringVectorWriter : public ResultWriter {
 public:
  void WriteCell(const std::string &cell) override { values_.back().push_back(cell); }
  void WriteHeaderCell(const std::string &cell) override {}
  void BeginHeader() override {}
  void EndHeader() override {}
  void BeginRow() override { values_.emplace_back(); }
  void EndRow() override {}
  void BeginTable(bool simplified_output) override { values_.clear(); }
  void EndTable() override {}

  std::vector<std::vector<std::string>> values_;
};

/** @brief Render a detached result without consulting executor/database state. */
void RenderQueryResult(const QueryResult &result, ResultWriter &writer);

}  // namespace bumblebee
