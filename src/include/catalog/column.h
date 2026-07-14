//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column.h
//
// Identification: src/include/catalog/column.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "common/config.h"
#include "common/macros.h"
#include "fmt/format.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * One column of a Schema: a name and a LogicalType, plus the byte offset and
 * width it occupies once laid out in a row.
 */
class Column {
  friend class Schema;

 public:
  /**
   * @brief Construct a fixed-width column.
   *
   * @param column_name The column name.
   * @param type The column type. Must not be variable-length; use the other
   *             constructor for VARCHAR / LIST / ARRAY.
   */
  Column(std::string column_name, const LogicalType &type)
      : column_name_(std::move(column_name)), column_type_(type), length_(StorageSizeOf(type, 0)) {
    BUMBLEBEE_ASSERT(type.IsConstantSize(),
                     "variable-length type needs the (name, type, length) constructor");
  }

  /**
   * @brief Construct a variable-length column (VARCHAR, LIST, ARRAY).
   *
   * @param column_name The column name.
   * @param type The column type.
   * @param length The declared length, e.g. the N in VARCHAR(N).
   */
  Column(std::string column_name, const LogicalType &type, uint32_t length)
      : column_name_(std::move(column_name)), column_type_(type), length_(StorageSizeOf(type, length)) {}

  /**
   * @brief Copy a column under a different name.
   *
   * @param column_name The new name.
   * @param column The column to copy the type and layout from.
   */
  Column(std::string column_name, const Column &column)
      : column_name_(std::move(column_name)),
        column_type_(column.column_type_),
        length_(column.length_),
        column_offset_(column.column_offset_) {}

  /**
   * @brief Construct a column of any type, fixed- or variable-length.
   *
   * The two public constructors above deliberately disagree about whether a length
   * is required, which is awkward when the type is only known at run time (a
   * constant lifted out of the parse tree, an inferred expression result). This
   * picks the right one, defaulting variable-length columns to VARCHAR_DEFAULT_LENGTH.
   *
   * @param column_name The column name.
   * @param type The column type.
   * @return Column The column.
   */
  static auto Make(std::string column_name, const LogicalType &type) -> Column {
    if (type.IsConstantSize()) {
      return Column{std::move(column_name), type};
    }
    return Column{std::move(column_name), type, VARCHAR_DEFAULT_LENGTH};
  }

  /** @return A copy of this column under a different name. */
  auto WithColumnName(std::string column_name) const -> Column {
    Column c = *this;
    c.column_name_ = std::move(column_name);
    return c;
  }

  /** @return The column name. */
  auto GetName() const -> std::string { return column_name_; }

  /** @return The number of bytes one value of this column occupies in a row. */
  auto GetStorageSize() const -> uint32_t { return length_; }

  /** @return This column's byte offset within a row. */
  auto GetOffset() const -> uint32_t { return column_offset_; }

  /** @return The column type. */
  auto GetType() const -> const LogicalType & { return column_type_; }

  /** @return True if values of this column are stored inline in the row. */
  auto IsInlined() const -> bool { return column_type_.IsConstantSize(); }

  /**
   * @brief Render this column.
   *
   * @param simplified When true, print just `name:TYPE`; otherwise print the full layout.
   * @return std::string The rendering.
   */
  auto ToString(bool simplified = true) const -> std::string;

 private:
  /**
   * @brief The number of bytes a value of `type` occupies inline in a row.
   *
   * Variable-length payloads (VARCHAR, LIST, ARRAY) live outside the row, so what
   * sits inline is a handle, not the data.
   */
  static auto StorageSizeOf(const LogicalType &type, uint32_t length) -> uint32_t {
    const auto physical = type.GetPhysicalType();
    if (type.IsConstantSize()) {
      return static_cast<uint32_t>(type.GetStorageSize());
    }
    switch (physical) {
      case PhysicalType::STRING:
      case PhysicalType::LIST:
      case PhysicalType::ARRAY:
        // The declared length is metadata; the row itself stores a handle.
        return length;
      default:
        UNREACHABLE("cannot size an invalid column type");
    }
  }

  /** Column name. */
  std::string column_name_;

  /** Column type. */
  LogicalType column_type_;

  /** The number of bytes this column occupies in a row. */
  uint32_t length_;

  /** This column's offset within a row. */
  uint32_t column_offset_{0};
};

}  // namespace bumblebee

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::Column, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const bumblebee::Column &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x.ToString(), ctx);
  }
};
