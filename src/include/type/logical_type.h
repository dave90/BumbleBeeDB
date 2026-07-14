//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// logical_type.h
//
// Identification: src/include/type/logical_type.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "common/config.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * The in-memory representation a value is actually stored as.
 *
 * Several logical types share one physical type: BOOLEAN is physically a UTINYINT,
 * DATE an INTEGER, TIMESTAMP a BIGINT, and a DECIMAL is one of SMALLINT / INTEGER /
 * BIGINT depending on its declared width.
 */
enum class PhysicalType : uint16_t {
  TINYINT = 1,
  SMALLINT = 2,
  INTEGER = 3,
  BIGINT = 4,
  UTINYINT = 5,
  USMALLINT = 6,
  UINTEGER = 7,
  UBIGINT = 8,

  FLOAT = 80,
  DOUBLE = 81,

  STRING = 100,

  STRUCT = 110,
  /** A variable-length sequence of a single child type. */
  LIST = 111,
  /** A fixed-length sequence of a single child type. */
  ARRAY = 112,

  UNKNOWN = 999
};

/** The SQL-level type of a value: what the binder resolves a column or literal to. */
enum class LogicalTypeId : uint16_t {
  BOOLEAN = 0,
  TINYINT = 1,
  SMALLINT = 2,
  INTEGER = 3,
  BIGINT = 4,
  UTINYINT = 5,
  USMALLINT = 6,
  UINTEGER = 7,
  UBIGINT = 8,
  /** A 64-bit hash value. Physically a UBIGINT. */
  HASH = 9,
  /** A row address. Physically a UBIGINT. */
  ADDRESS = 10,

  FLOAT = 80,
  DOUBLE = 81,

  DECIMAL = 90,

  STRING = 100,

  STRUCT = 110,
  LIST = 111,
  ARRAY = 112,

  DATE = 200,
  TIMESTAMP = 201,

  UNKNOWN = 999
};

class LogicalType;

/**
 * Extra data carried by the complex logical types. A scalar type has none.
 */
struct LogicalTypeExtraData {
  virtual ~LogicalTypeExtraData() = default;
  /** @return True if `other` is the same kind of extra data with the same contents. */
  virtual auto Equals(const LogicalTypeExtraData &other) const -> bool = 0;
};

/** The width and scale of a DECIMAL. */
struct DecimalTypeData : public LogicalTypeExtraData {
  DecimalTypeData(int width, int scale) : width_(width), scale_(scale) {}
  auto Equals(const LogicalTypeExtraData &other) const -> bool override;

  int width_;
  int scale_;
};

/** The element type of a LIST, and — for an ARRAY — how many of them there are. */
struct ListTypeData : public LogicalTypeExtraData {
  /** @param size The fixed element count of an ARRAY; 0 for a variable-length LIST. */
  ListTypeData(const LogicalType &child_type, idx_t size);
  auto Equals(const LogicalTypeExtraData &other) const -> bool override;

  std::shared_ptr<LogicalType> child_type_;
  idx_t size_;
};

/**
 * A SQL type: a LogicalTypeId, the PhysicalType it is stored as, and — for the
 * complex types (DECIMAL, LIST, ARRAY) — a shared payload describing it further.
 */
class LogicalType {
 public:
  /** The widest DECIMAL that fits in each backing integer width. */
  static constexpr int MAX_DECIMAL_WIDTH_INT16 = 4;
  static constexpr int MAX_DECIMAL_WIDTH_INT32 = 9;
  static constexpr int MAX_DECIMAL_WIDTH_INT64 = 18;

  LogicalType() = default;

  /** @brief Construct the logical type for the given id. */
  LogicalType(LogicalTypeId type);  // NOLINT(google-explicit-constructor): implicit is intended

  /** @brief Construct the plain logical type backed by the given physical type. */
  LogicalType(PhysicalType type);  // NOLINT(google-explicit-constructor)

  // -- Factories ------------------------------------------------------------

  /**
   * @brief Create a DECIMAL. The backing physical type widens with the precision.
   *
   * @param width The total number of digits. At most MAX_DECIMAL_WIDTH_INT64.
   * @param scale The number of digits after the decimal point.
   * @return LogicalType The DECIMAL type.
   */
  static auto Decimal(int width, int scale) -> LogicalType;

  /**
   * @brief Create a variable-length LIST, e.g. `INTEGER[]`.
   *
   * @param child_type The element type.
   * @return LogicalType The LIST type.
   */
  static auto List(const LogicalType &child_type) -> LogicalType;

  /**
   * @brief Create a fixed-length ARRAY, e.g. `INTEGER[3]`.
   *
   * @param child_type The element type.
   * @param size The fixed number of elements.
   * @return LogicalType The ARRAY type.
   */
  static auto Array(const LogicalType &child_type, idx_t size) -> LogicalType;

  static auto Date() -> LogicalType { return {LogicalTypeId::DATE}; }
  static auto Timestamp() -> LogicalType { return {LogicalTypeId::TIMESTAMP}; }

  /**
   * @brief Resolve a SQL type name from the DDL (e.g. "int4", "varchar") to a type.
   *
   * @param type_name The type name, in any case.
   * @return LogicalType The resolved type, or UNKNOWN if unrecognized.
   */
  static auto FromString(const std::string &type_name) -> LogicalType;

  /**
   * @brief The type both operands of a binary operation should be promoted to.
   *
   * @param lhs The left operand type.
   * @param rhs The right operand type.
   * @return LogicalType The common type.
   */
  static auto CommonType(const LogicalType &lhs, const LogicalType &rhs) -> LogicalType;

  // -- Accessors ------------------------------------------------------------

  /** @return The logical type id. */
  auto GetTypeId() const -> LogicalTypeId { return type_; }

  /** @return The physical type this logical type is stored as. */
  auto GetPhysicalType() const -> PhysicalType { return ctype_; }

  /** @return True if every value of this type occupies the same number of bytes. */
  auto IsConstantSize() const -> bool { return IsConstantSize(ctype_); }

  /**
   * @return idx_t The number of bytes one value occupies inline in a row. Throws for
   *         the variable-length types, whose payload lives outside the row.
   */
  auto GetStorageSize() const -> idx_t { return SizeOf(ctype_); }

  /** @return A rendering such as "INTEGER", "DECIMAL(10,2)", "INTEGER[]", "INTEGER[3]". */
  auto ToString() const -> std::string;

  /** @return The width and scale. Only valid when GetTypeId() == DECIMAL. */
  auto GetDecimalData() const -> const DecimalTypeData &;

  /** @return The element type and size. Only valid for LIST and ARRAY. */
  auto GetListData() const -> const ListTypeData &;

  /** @return The element type. Only valid for LIST and ARRAY. */
  auto GetChildType() const -> const LogicalType &;

  // -- Helpers over the bare enums ------------------------------------------

  /** @brief The physical type a logical type is stored as. */
  static auto PhysicalTypeOf(LogicalTypeId type) -> PhysicalType;

  /** @brief The logical type that plainly represents a physical type. */
  static auto LogicalTypeIdOf(PhysicalType type) -> LogicalTypeId;

  /** @brief The size in bytes of one value of a fixed-width physical type. */
  static auto SizeOf(PhysicalType type) -> idx_t;

  /** @brief True if every value of this physical type is the same number of bytes. */
  static auto IsConstantSize(PhysicalType type) -> bool;

  static auto NameOf(PhysicalType type) -> std::string;
  static auto NameOf(LogicalTypeId type) -> std::string;

  friend auto operator==(const LogicalType &lhs, const LogicalType &rhs) -> bool;
  friend auto operator!=(const LogicalType &lhs, const LogicalType &rhs) -> bool;

 private:
  PhysicalType ctype_{PhysicalType::UNKNOWN};
  LogicalTypeId type_{LogicalTypeId::UNKNOWN};
  /** Payload for the complex types (DECIMAL, LIST, ARRAY). Null for scalars. */
  std::shared_ptr<LogicalTypeExtraData> data_;
};

}  // namespace bumblebee

template <>
struct fmt::formatter<bumblebee::LogicalType> : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const bumblebee::LogicalType &t, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(t.ToString(), ctx);
  }
};
