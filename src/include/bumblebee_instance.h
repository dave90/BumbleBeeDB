//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumblebee_instance.h
//
// Identification: src/include/bumblebee_instance.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "catalog/catalog.h"

namespace bumblebee {

class CreateStatement;
class ExplainStatement;

/**
 * Where a statement's results are written. The instance drives this interface; the
 * shell, the tests and any future frontend each supply their own implementation.
 */
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

/** @brief Writes separator-delimited plain text to a stream. Used by the shell. */
class SimpleStreamWriter : public ResultWriter {
 public:
  explicit SimpleStreamWriter(std::ostream &stream, bool disable_header = false, std::string separator = "\t")
      : disable_header_(disable_header), stream_(stream), separator_(std::move(separator)) {}

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

  bool disable_header_;
  std::ostream &stream_;
  std::string separator_;
};

/** @brief Collects the cells into a vector of rows. Used by the tests. */
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

/**
 * A BumbleBeeDB database.
 *
 * This drives the whole SQL frontend: parse, bind, plan, optimize. There is no
 * execution engine yet, so a query is answered by printing the plan it would have
 * run; `CREATE TABLE` is the one statement that has a real effect, registering a
 * schema in the catalog.
 */
class BumbleBeeInstance {
 public:
  BumbleBeeInstance();
  ~BumbleBeeInstance();

  /**
   * @brief Parse, bind, plan and optimize a SQL string, writing the result to `writer`.
   *
   * @param sql One or more `;`-separated statements, or a `\`-prefixed meta-command.
   * @param writer Where the result goes.
   * @return bool True if every statement succeeded.
   */
  auto ExecuteSql(const std::string &sql, ResultWriter &writer) -> bool;

  /** @brief Register a few tables so a fresh shell has something to query. */
  void GenerateMockTable();

  /** The catalog. */
  std::unique_ptr<Catalog> catalog_;

 private:
  void HandleCreateStatement(const CreateStatement &stmt, ResultWriter &writer);
  void HandleExplainStatement(const ExplainStatement &stmt, ResultWriter &writer);
  void CmdDisplayTables(ResultWriter &writer);
  void CmdDisplayHelp(ResultWriter &writer);
  void WriteOneCell(const std::string &cell, ResultWriter &writer);
};

}  // namespace bumblebee
