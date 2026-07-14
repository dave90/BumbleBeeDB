//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_order_by.h
//
// Identification: src/include/binder/bound_order_by.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "binder/bound_expression.h"
#include "common/exception.h"
#include "execution/expressions/abstract_expression.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * The sort direction of one ORDER BY item.
 */
enum class OrderByType : uint8_t {
  /** Invalid. */
  INVALID = 0,
  /** No direction given; the planner picks one. */
  DEFAULT = 1,
  /** ASC. */
  ASC = 2,
  /** DESC. */
  DESC = 3,
};

/**
 * Where NULLs sort in one ORDER BY item.
 */
enum class OrderByNullType : uint8_t {
  /** No null ordering given. */
  DEFAULT = 0,
  /** NULLS FIRST. */
  NULLS_FIRST = 1,
  /** NULLS LAST. */
  NULLS_LAST = 2,
};

/** One ORDER BY item, once the expression has been planned. */
using OrderBy = std::tuple<OrderByType, OrderByNullType, AbstractExpressionRef>;

}  // namespace bumblebee

// The formatters below must be declared before BoundOrderBy::ToString(), which
// formats an OrderByType and an OrderByNullType inline.

template <>
struct fmt::formatter<bumblebee::OrderByType> : fmt::formatter<fmt::string_view> {
  template <typename FormatCtx>
  auto format(bumblebee::OrderByType c, FormatCtx &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::OrderByType::INVALID:
        name = "Invalid";
        break;
      case bumblebee::OrderByType::DEFAULT:
        name = "Default";
        break;
      case bumblebee::OrderByType::ASC:
        name = "Ascending";
        break;
      case bumblebee::OrderByType::DESC:
        name = "Descending";
        break;
      default:
        name = "Unknown";
        break;
    }
    return fmt::formatter<fmt::string_view>::format(name, ctx);
  }
};

template <>
struct fmt::formatter<bumblebee::OrderByNullType> : fmt::formatter<fmt::string_view> {
  template <typename FormatCtx>
  auto format(bumblebee::OrderByNullType c, FormatCtx &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::OrderByNullType::DEFAULT:
        name = "Default";
        break;
      case bumblebee::OrderByNullType::NULLS_FIRST:
        name = "NullsFirst";
        break;
      case bumblebee::OrderByNullType::NULLS_LAST:
        name = "NullsLast";
        break;
      default:
        name = "Unknown";
        break;
    }
    return fmt::formatter<fmt::string_view>::format(name, ctx);
  }
};

namespace bumblebee {

/**
 * One item of an ORDER BY clause.
 */
class BoundOrderBy {
 public:
  /**
   * @brief Construct one ORDER BY item.
   *
   * @param type The sort direction.
   * @param null_order Where NULLs sort.
   * @param expr The expression to sort on.
   */
  BoundOrderBy(OrderByType type, OrderByNullType null_order, std::unique_ptr<BoundExpression> expr)
      : type_(type), null_order_(null_order), expr_(std::move(expr)) {}

  /** The sort direction. */
  OrderByType type_;

  /** Where NULLs sort. */
  OrderByNullType null_order_;

  /** The expression to sort on. */
  std::unique_ptr<BoundExpression> expr_;

  /** @return A human-readable rendering of this ORDER BY item. */
  auto ToString() const -> std::string {
    return fmt::format("BoundOrderBy {{ type={}, nulls={}, expr={} }}", type_, null_order_, expr_);
  }
};

}  // namespace bumblebee

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::BoundOrderBy, T>::value, char>>
    : fmt::formatter<std::string> {
  /**
   * Hide the base's set_debug_format(). fmt >= 10 puts range elements into "debug"
   * mode, which would quote and escape every rendering. A bound tree is printed for
   * humans, not re-parsed, so we opt out and print it plain.
   */
  FMT_CONSTEXPR void set_debug_format(bool /*set*/ = true) {}

  template <typename FormatCtx>
  auto format(const bumblebee::BoundOrderBy &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x.ToString(), ctx);
  }
};

template <typename T>
struct fmt::formatter<std::unique_ptr<T>, std::enable_if_t<std::is_base_of<bumblebee::BoundOrderBy, T>::value, char>>
    : fmt::formatter<std::string> {
  /**
   * Hide the base's set_debug_format(). fmt >= 10 puts range elements into "debug"
   * mode, which would quote and escape every rendering. A bound tree is printed for
   * humans, not re-parsed, so we opt out and print it plain.
   */
  FMT_CONSTEXPR void set_debug_format(bool /*set*/ = true) {}

  template <typename FormatCtx>
  auto format(const std::unique_ptr<bumblebee::BoundOrderBy> &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x->ToString(), ctx);
  }
};
