//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_expression.h
//
// Identification: src/include/binder/bound_expression.h
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
 * The kinds of expression the binder produces.
 */
enum class ExpressionType : uint8_t {
  /** Invalid expression. */
  INVALID = 0,
  /** A literal, e.g. `1`. */
  CONSTANT = 1,
  /** A reference to a column of a table in scope. */
  COLUMN_REF = 3,
  /** A cast. */
  TYPE_CAST = 4,
  /** A built-in function. */
  FUNCTION = 5,
  /** An aggregate call, e.g. `sum(x)`. */
  AGG_CALL = 6,
  /** The `*` in a SELECT list. Rewritten by the binder; never reaches the plan. */
  STAR = 7,
  /** A unary operator, e.g. `-x`. */
  UNARY_OP = 8,
  /** A binary operator, e.g. `a + b`. */
  BINARY_OP = 9,
  /** An alias, e.g. `x AS y`. */
  ALIAS = 10,
  /** A scalar function call, e.g. `lower(x)`. */
  FUNC_CALL = 11,
};

/**
 * A bound expression: a node of the parse tree resolved against the catalog and
 * the tables currently in scope.
 */
class BoundExpression {
 public:
  /**
   * @brief Construct a bound expression of the given kind.
   *
   * @param type The expression type.
   */
  explicit BoundExpression(ExpressionType type) : type_(type) {}

  BoundExpression() = default;
  virtual ~BoundExpression() = default;

  /** @return A human-readable rendering of this expression. */
  virtual auto ToString() const -> std::string { return ""; }

  /** @return True if this is the placeholder for an absent clause. */
  auto IsInvalid() const -> bool { return type_ == ExpressionType::INVALID; }

  /** @return True if this expression contains an aggregate call. */
  virtual auto HasAggregation() const -> bool { UNREACHABLE("HasAggregation should have been implemented"); }

  /** The type of this expression. */
  ExpressionType type_{ExpressionType::INVALID};
};

}  // namespace bumblebee

template <>
struct fmt::formatter<bumblebee::ExpressionType> : fmt::formatter<fmt::string_view> {
  template <typename FormatCtx>
  auto format(bumblebee::ExpressionType c, FormatCtx &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::ExpressionType::INVALID:
        name = "Invalid";
        break;
      case bumblebee::ExpressionType::CONSTANT:
        name = "Constant";
        break;
      case bumblebee::ExpressionType::COLUMN_REF:
        name = "ColumnRef";
        break;
      case bumblebee::ExpressionType::TYPE_CAST:
        name = "TypeCast";
        break;
      case bumblebee::ExpressionType::FUNCTION:
        name = "Function";
        break;
      case bumblebee::ExpressionType::AGG_CALL:
        name = "AggregationCall";
        break;
      case bumblebee::ExpressionType::STAR:
        name = "Star";
        break;
      case bumblebee::ExpressionType::UNARY_OP:
        name = "UnaryOperation";
        break;
      case bumblebee::ExpressionType::BINARY_OP:
        name = "BinaryOperation";
        break;
      case bumblebee::ExpressionType::ALIAS:
        name = "Alias";
        break;
      case bumblebee::ExpressionType::FUNC_CALL:
        name = "FuncCall";
        break;
      default:
        name = "Unknown";
        break;
    }
    return fmt::formatter<fmt::string_view>::format(name, ctx);
  }
};

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::BoundExpression, T>::value, char>>
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
struct fmt::formatter<std::unique_ptr<T>,
                      std::enable_if_t<std::is_base_of<bumblebee::BoundExpression, T>::value, char>>
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
