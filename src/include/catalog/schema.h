//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// schema.h
//
// Identification: src/include/catalog/schema.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/column.h"
#include "common/exception.h"
#include "common/macros.h"

namespace bumblebee {

class Schema;
using SchemaRef = std::shared_ptr<const Schema>;

/**
 * The ordered list of columns a tuple has.
 */
class Schema {
 public:
  /**
   * @brief Construct a schema from its columns, left to right.
   *
   * Column offsets are assigned here.
   *
   * @param columns The columns.
   */
  explicit Schema(const std::vector<Column> &columns);

  /**
   * @brief Build a schema from a subset of another schema's columns.
   *
   * @param from The source schema.
   * @param attrs The indices of the columns to keep, in the order to keep them.
   * @return Schema The projected schema.
   */
  static auto CopySchema(const Schema *from, const std::vector<uint32_t> &attrs) -> Schema {
    std::vector<Column> cols;
    cols.reserve(attrs.size());
    for (const auto i : attrs) {
      cols.emplace_back(from->columns_[i]);
    }
    return Schema{cols};
  }

  /** @return All the columns in this schema. */
  auto GetColumns() const -> const std::vector<Column> & { return columns_; }

  /**
   * @brief The column types, in schema order.
   *
   * This is the shape every DataChunk-producing operator needs to size its output, so it is
   * built here rather than re-derived per operator.
   *
   * @return std::vector<LogicalType> One entry per column.
   */
  [[nodiscard]] auto GetTypes() const -> std::vector<LogicalType> {
    std::vector<LogicalType> types;
    types.reserve(columns_.size());
    for (const auto &col : columns_) {
      types.push_back(col.GetType());
    }
    return types;
  }

  /**
   * @brief Get the column at `col_idx`.
   *
   * @param col_idx The column index.
   * @return const Column& The column.
   */
  auto GetColumn(uint32_t col_idx) const -> const Column & { return columns_[col_idx]; }

  /**
   * @brief The index of the first column named `col_name`.
   *
   * @param col_name The column name.
   * @return uint32_t The index. Throws if no such column exists.
   */
  auto GetColIdx(const std::string &col_name) const -> uint32_t {
    if (auto col_idx = TryGetColIdx(col_name)) {
      return *col_idx;
    }
    throw Exception(ExceptionType::BINDER, fmt::format("column {} does not exist", col_name));
  }

  /**
   * @brief The index of the first column named `col_name`, if there is one.
   *
   * @param col_name The column name.
   * @return std::optional<uint32_t> The index, or nullopt.
   */
  [[nodiscard]] auto TryGetColIdx(const std::string &col_name) const -> std::optional<uint32_t> {
    for (uint32_t i = 0; i < columns_.size(); ++i) {
      if (columns_[i].GetName() == col_name) {
        return std::optional{i};
      }
    }
    return std::nullopt;
  }

  /** @return The number of columns. */
  auto GetColumnCount() const -> uint32_t { return static_cast<uint32_t>(columns_.size()); }

  /** @return The number of columns whose payloads live outside the row. */
  auto GetUnlinedColumnCount() const -> uint32_t { return static_cast<uint32_t>(uninlined_columns_.size()); }

  /** @return The number of bytes one row of this schema occupies inline. */
  auto GetInlinedStorageSize() const -> uint32_t { return length_; }

  /** @return True if every column is stored inline. */
  auto IsInlined() const -> bool { return tuple_is_inlined_; }

  /**
   * @brief Render this schema.
   *
   * @param simplified When true, print `(a:INTEGER, b:VARCHAR)`; otherwise print the full layout.
   * @return std::string The rendering.
   */
  auto ToString(bool simplified = true) const -> std::string;

 private:
  /** The number of bytes one row occupies inline. */
  uint32_t length_{0};

  /** All the columns, inlined and uninlined. */
  std::vector<Column> columns_;

  /** True if every column is stored inline. */
  bool tuple_is_inlined_{true};

  /** The indices of the uninlined columns. */
  std::vector<uint32_t> uninlined_columns_;
};

}  // namespace bumblebee

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::Schema, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const bumblebee::Schema &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x.ToString(), ctx);
  }
};

template <typename T>
struct fmt::formatter<std::shared_ptr<T>, std::enable_if_t<std::is_base_of<bumblebee::Schema, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const std::shared_ptr<T> &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x->ToString(), ctx);
  }
};
