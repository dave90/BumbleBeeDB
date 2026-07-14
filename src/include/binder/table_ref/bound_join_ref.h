//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_join_ref.h
//
// Identification: src/include/binder/table_ref/bound_join_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "binder/bound_expression.h"
#include "binder/bound_table_ref.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * The kinds of join.
 */
enum class JoinType : uint8_t {
  /** Invalid join. */
  INVALID = 0,
  /** LEFT OUTER JOIN. */
  LEFT = 1,
  /** RIGHT OUTER JOIN. */
  RIGHT = 3,
  /** INNER JOIN. */
  INNER = 4,
  /** FULL OUTER JOIN. */
  OUTER = 5
};

}  // namespace bumblebee

// Must be declared before BoundJoinRef::ToString(), which formats a JoinType inline.
template <>
struct fmt::formatter<bumblebee::JoinType> : fmt::formatter<fmt::string_view> {
  template <typename FormatCtx>
  auto format(bumblebee::JoinType c, FormatCtx &ctx) const {
    fmt::string_view name;
    switch (c) {
      case bumblebee::JoinType::INVALID:
        name = "Invalid";
        break;
      case bumblebee::JoinType::LEFT:
        name = "Left";
        break;
      case bumblebee::JoinType::RIGHT:
        name = "Right";
        break;
      case bumblebee::JoinType::INNER:
        name = "Inner";
        break;
      case bumblebee::JoinType::OUTER:
        name = "Outer";
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
 * A join, e.g. the `x INNER JOIN y ON ...` in `SELECT * FROM x INNER JOIN y ON ...`.
 */
class BoundJoinRef : public BoundTableRef {
 public:
  /**
   * @brief Construct a join.
   *
   * @param join_type The kind of join.
   * @param left The left side.
   * @param right The right side.
   * @param condition The ON condition.
   */
  explicit BoundJoinRef(JoinType join_type, std::unique_ptr<BoundTableRef> left, std::unique_ptr<BoundTableRef> right,
                        std::unique_ptr<BoundExpression> condition)
      : BoundTableRef(TableReferenceType::JOIN),
        join_type_(join_type),
        left_(std::move(left)),
        right_(std::move(right)),
        condition_(std::move(condition)) {}

  auto ToString() const -> std::string override {
    return fmt::format("BoundJoin {{ type={}, left={}, right={}, condition={} }}", join_type_, left_, right_,
                       condition_);
  }

  /** The kind of join. */
  JoinType join_type_;

  /** The left side of the join. */
  std::unique_ptr<BoundTableRef> left_;

  /** The right side of the join. */
  std::unique_ptr<BoundTableRef> right_;

  /** The ON condition. */
  std::unique_ptr<BoundExpression> condition_;
};

}  // namespace bumblebee
