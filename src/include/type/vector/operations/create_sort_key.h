//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// create_sort_key.h
//
// Identification: src/include/type/vector/operations/create_sort_key.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/exception.h"
#include "common/util/string_util.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** The direction of an ORDER BY key. */
enum class OrderType : uint8_t { INVALID = 0, ASCENDING = 2, DESCENDING = 3 };

/** How one ORDER BY key is to be ordered. */
struct OrderModifiers {
  OrderModifiers(OrderType order_type) : order_type_(order_type) {}  // NOLINT(google-explicit-constructor)

  OrderType order_type_;

  auto operator==(const OrderModifiers &other) const -> bool { return order_type_ == other.order_type_; }

  /** @brief Parse "asc" / "desc" (in any case) into an OrderModifiers. */
  static auto Parse(const std::string &val) -> OrderModifiers {
    auto lcase = StringUtil::Lower(val);
    if (StringUtil::StartsWith(lcase, "asc")) {
      return OrderModifiers(OrderType::ASCENDING);
    }
    if (StringUtil::StartsWith(lcase, "desc")) {
      return OrderModifiers(OrderType::DESCENDING);
    }
    throw NotImplementedException("create_sort_key: the modifier must start with either ASC or DESC");
  }

  /** @return "ASC", "DESC", or the empty string. */
  auto ToString() const -> std::string {
    switch (order_type_) {
      case OrderType::ASCENDING:
        return "ASC";
      case OrderType::DESCENDING:
        return "DESC";
      default:
        return "";
    }
  }
};

/**
 * Encodes the ORDER BY columns of a chunk into one byte-comparable STRING per row.
 *
 * Sorting then compares the encoded blobs with memcmp — no per-row type dispatch, no
 * per-column branch — and a DESCENDING key is just the same encoding with every byte
 * flipped. NULLs are encoded explicitly (see the operators in create_sort_key.cpp), and
 * land LAST in ASC / FIRST in DESC, which is the conventional SQL default.
 */
struct CreateSortKey {
  /**
   * @brief Encode every row of `input` into one sort key.
   *
   * @param input The columns to order by, in key order.
   * @param modifiers One per column: ASC or DESC.
   * @param result The STRING vector to fill with the keys.
   */
  static void Create(DataChunk &input, const std::vector<OrderModifiers> &modifiers, Vector &result);

  /** @brief Encode a single column. */
  static void Create(Vector &input, idx_t size, const OrderModifiers &modifiers, Vector &result);
};

}  // namespace bumblebee
