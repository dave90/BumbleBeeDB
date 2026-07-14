//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_cross_product_ref.h
//
// Identification: src/include/binder/table_ref/bound_cross_product_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "binder/bound_table_ref.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * A cartesian product, e.g. the `x, y` in `SELECT * FROM x, y`.
 */
class BoundCrossProductRef : public BoundTableRef {
 public:
  /**
   * @brief Construct a cross product.
   *
   * @param left The left side.
   * @param right The right side.
   */
  explicit BoundCrossProductRef(std::unique_ptr<BoundTableRef> left, std::unique_ptr<BoundTableRef> right)
      : BoundTableRef(TableReferenceType::CROSS_PRODUCT), left_(std::move(left)), right_(std::move(right)) {}

  auto ToString() const -> std::string override {
    return fmt::format("BoundCrossProductRef {{ left={}, right={} }}", left_, right_);
  }

  /** The left side of the cross product. */
  std::unique_ptr<BoundTableRef> left_;

  /** The right side of the cross product. */
  std::unique_ptr<BoundTableRef> right_;
};

}  // namespace bumblebee
