//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_table_ref.h
//
// Identification: src/include/binder/bound_table_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "common/macros.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * The kinds of table reference the binder produces for a FROM clause.
 */
enum class TableReferenceType : uint8_t {
  /** Invalid table reference. */
  INVALID = 0,
  /** A table in the catalog. */
  BASE_TABLE = 1,
  /** The output of a join. */
  JOIN = 3,
  /** The output of a cartesian product. */
  CROSS_PRODUCT = 4,
  /** A VALUES clause. */
  EXPRESSION_LIST = 5,
  /** A subquery in the FROM clause. */
  SUBQUERY = 6,
  /** A reference to a CTE. */
  CTE = 7,
  /** Placeholder for an empty FROM clause. */
  EMPTY = 8
};

/**
 * A bound table reference: one entry of a FROM clause, resolved against the catalog.
 */
class BoundTableRef {
 public:
  /**
   * @brief Construct a bound table reference of the given kind.
   *
   * @param type The table reference type.
   */
  explicit BoundTableRef(TableReferenceType type) : type_(type) {}

  BoundTableRef() = default;
  virtual ~BoundTableRef() = default;

  /** @return A human-readable rendering of this table reference. */
  virtual auto ToString() const -> std::string {
    switch (type_) {
      case TableReferenceType::INVALID:
        return "";
      case TableReferenceType::EMPTY:
        return "<empty>";
      default:
        // Every other kind of table reference overrides ToString().
        UNREACHABLE("entered unreachable code");
    }
  }

  /** @return True if this is the placeholder for an absent FROM clause. */
  auto IsInvalid() const -> bool { return type_ == TableReferenceType::INVALID; }

  /** The type of this table reference. */
  TableReferenceType type_{TableReferenceType::INVALID};
};

}  // namespace bumblebee

template <>
struct fmt::formatter<bumblebee::TableReferenceType> : fmt::formatter<fmt::string_view> {
  template <typename FormatCtx>
  auto format(bumblebee::TableReferenceType c, FormatCtx &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::TableReferenceType::INVALID:
        name = "Invalid";
        break;
      case bumblebee::TableReferenceType::BASE_TABLE:
        name = "BaseTable";
        break;
      case bumblebee::TableReferenceType::JOIN:
        name = "Join";
        break;
      case bumblebee::TableReferenceType::CROSS_PRODUCT:
        name = "CrossProduct";
        break;
      case bumblebee::TableReferenceType::EXPRESSION_LIST:
        name = "ExpressionList";
        break;
      case bumblebee::TableReferenceType::SUBQUERY:
        name = "Subquery";
        break;
      case bumblebee::TableReferenceType::CTE:
        name = "CTE";
        break;
      case bumblebee::TableReferenceType::EMPTY:
        name = "Empty";
        break;
      default:
        name = "Unknown";
        break;
    }
    return fmt::formatter<fmt::string_view>::format(name, ctx);
  }
};

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::BoundTableRef, T>::value, char>>
    : fmt::formatter<std::string> {
  /**
   * Hide the base's set_debug_format(). fmt >= 10 puts range elements into "debug"
   * mode, which would quote and escape every rendering. A bound tree is printed for
   * humans, not re-parsed, so we opt out and print it plain.
   */
  FMT_CONSTEXPR void set_debug_format(bool /*set*/ = true) {}

  template <typename FormatCtx>
  auto format(const T &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x.ToString(), ctx);
  }
};

template <typename T>
struct fmt::formatter<std::unique_ptr<T>, std::enable_if_t<std::is_base_of<bumblebee::BoundTableRef, T>::value, char>>
    : fmt::formatter<std::string> {
  /**
   * Hide the base's set_debug_format(). fmt >= 10 puts range elements into "debug"
   * mode, which would quote and escape every rendering. A bound tree is printed for
   * humans, not re-parsed, so we opt out and print it plain.
   */
  FMT_CONSTEXPR void set_debug_format(bool /*set*/ = true) {}

  template <typename FormatCtx>
  auto format(const std::unique_ptr<T> &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x->ToString(), ctx);
  }
};
