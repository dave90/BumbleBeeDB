//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// value.h
//
// Identification: src/include/type/value.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "fmt/format.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * A single boxed SQL value.
 *
 * A Value carries its full LogicalType, not just its physical type — a SQL
 * BOOLEAN and a UTINYINT share a physical representation but are not the same
 * value, and the planner relies on being able to tell them apart (it emits a
 * BOOLEAN constant `true` as a cross-product predicate, which the optimizer then
 * recognizes and eliminates).
 *
 * Value is deliberately a boundary type: literals lifted out of the parse tree,
 * constant folding, and printing. It is not on any hot path, so it holds a plain
 * std::string for its string payload. The execution engine will use BumbleString
 * over row-layout blocks instead.
 */
class Value {
 public:
  /** @brief Construct a typeless NULL. */
  Value() = default;

  explicit Value(bool val);
  Value(int8_t val);    // NOLINT(google-explicit-constructor)
  Value(int16_t val);   // NOLINT(google-explicit-constructor)
  Value(int32_t val);   // NOLINT(google-explicit-constructor)
  Value(int64_t val);   // NOLINT(google-explicit-constructor)
  Value(uint8_t val);   // NOLINT(google-explicit-constructor)
  Value(uint16_t val);  // NOLINT(google-explicit-constructor)
  Value(uint32_t val);  // NOLINT(google-explicit-constructor)
  Value(uint64_t val);  // NOLINT(google-explicit-constructor)
  Value(float val);     // NOLINT(google-explicit-constructor)
  Value(double val);    // NOLINT(google-explicit-constructor)
  Value(std::string val);  // NOLINT(google-explicit-constructor)
  Value(const char *val);  // NOLINT(google-explicit-constructor)

  /**
   * @brief Construct a typed NULL.
   *
   * NULL is carried by the is_null_ flag, never by a sentinel stored in the union,
   * so a NULL still knows what type it would have been.
   *
   * @param type The type this NULL would have had.
   * @return Value The NULL value.
   */
  static auto Null(const LogicalType &type = LogicalTypeId::UNKNOWN) -> Value;

  /**
   * @brief Construct a LIST or ARRAY value from its elements.
   *
   * @param type The LIST or ARRAY type.
   * @param children The element values.
   * @return Value The composite value.
   */
  static auto List(const LogicalType &type, std::vector<Value> children) -> Value;

  /** @return True if this is SQL NULL. */
  auto IsNull() const -> bool { return is_null_; }

  /** @return The logical type of this value. */
  auto GetType() const -> const LogicalType & { return type_; }

  /** @return The physical type this value is stored as. */
  auto GetPhysicalType() const -> PhysicalType { return type_.GetPhysicalType(); }

  /** @return The elements of this LIST or ARRAY value. Empty for every scalar. */
  auto GetChildren() const -> const std::vector<Value> & { return children_; }

  /** @return A SQL-ish rendering of this value, e.g. `1`, `'abc'`, `NULL`, `[1, 2, 3]`. */
  auto ToString() const -> std::string;

  /**
   * @brief Read this value as `T`, converting between the numeric representations.
   *
   * @tparam T The arithmetic type to read as.
   * @return T The converted value.
   */
  template <typename T>
  auto GetAs() const -> T {
    switch (type_.GetPhysicalType()) {
      case PhysicalType::TINYINT:
        return static_cast<T>(value_.tinyint_);
      case PhysicalType::SMALLINT:
        return static_cast<T>(value_.smallint_);
      case PhysicalType::INTEGER:
        return static_cast<T>(value_.integer_);
      case PhysicalType::BIGINT:
        return static_cast<T>(value_.bigint_);
      case PhysicalType::UTINYINT:
        return static_cast<T>(value_.utinyint_);
      case PhysicalType::USMALLINT:
        return static_cast<T>(value_.usmallint_);
      case PhysicalType::UINTEGER:
        return static_cast<T>(value_.uinteger_);
      case PhysicalType::UBIGINT:
        return static_cast<T>(value_.ubigint_);
      case PhysicalType::FLOAT:
        return static_cast<T>(value_.float_);
      case PhysicalType::DOUBLE:
        return static_cast<T>(value_.double_);
      default:
        throw Exception(ExceptionType::MISMATCH_TYPE,
                        fmt::format("cannot read {} as a number", type_.ToString()));
    }
  }

  /** @return The string payload. Only valid when the physical type is STRING. */
  auto GetString() const -> const std::string &;

  /**
   * @brief Cast this value to `type`.
   *
   * @param type The target type.
   * @return Value The converted value.
   */
  auto CastAs(const LogicalType &type) const -> Value;

  friend auto operator==(const Value &lhs, const Value &rhs) -> bool;
  friend auto operator!=(const Value &lhs, const Value &rhs) -> bool;

 private:
  union Val {
    int8_t tinyint_;
    int16_t smallint_;
    int32_t integer_;
    int64_t bigint_;
    uint8_t utinyint_;
    uint16_t usmallint_;
    uint32_t uinteger_;
    uint64_t ubigint_;
    float float_;
    double double_;
  } value_{};

  /** The payload when the physical type is STRING. */
  std::string string_value_;

  /** The elements when the logical type is LIST, ARRAY or STRUCT. */
  std::vector<Value> children_;

  LogicalType type_{LogicalTypeId::UNKNOWN};

  bool is_null_{true};
};

}  // namespace bumblebee

template <>
struct fmt::formatter<bumblebee::Value> : formatter<std::string> {
  template <typename FormatCtx>
  auto format(const bumblebee::Value &v, FormatCtx &ctx) const {
    return formatter<std::string>::format(v.ToString(), ctx);
  }
};
