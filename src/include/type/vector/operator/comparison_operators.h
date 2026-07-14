//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// comparison_operators.h
//
// Identification: src/include/type/vector/operator/comparison_operators.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstring>

#include "type/bumble_string.h"

namespace bumblebee {

//===--------------------------------------------------------------------===//
// Comparison operations
//
// These are the VALUE comparisons only: they know nothing about NULL. The three-valued
// logic lives one level up, in BinaryExecution::Select, which never calls Operation() on
// a row where either side is NULL.
//===--------------------------------------------------------------------===//

/** @brief `left == right`. */
struct Equals {
  template <class T>
  static inline auto Operation(const T &left, const T &right) -> bool {
    return left == right;
  }
};

/** @brief `left != right`. */
struct NotEquals {
  template <class T>
  static inline auto Operation(const T &left, const T &right) -> bool {
    return left != right;
  }
};

/** @brief `left > right`. */
struct GreaterThan {
  template <class T>
  static inline auto Operation(const T &left, const T &right) -> bool {
    return left > right;
  }
};

/** @brief `left >= right`. */
struct GreaterThanEquals {
  template <class T>
  static inline auto Operation(const T &left, const T &right) -> bool {
    return left >= right;
  }
};

/** @brief `left < right`. */
struct LessThan {
  template <class T>
  static inline auto Operation(const T &left, const T &right) -> bool {
    return left < right;
  }
};

/** @brief `left <= right`. */
struct LessThanEquals {
  template <class T>
  static inline auto Operation(const T &left, const T &right) -> bool {
    return left <= right;
  }
};

//===--------------------------------------------------------------------===//
// Specialized boolean comparison operators
//
// A bool is stored as a byte, so the generic `<` / `>` would compare the raw bytes. Spell
// the ordering out instead: false < true.
//===--------------------------------------------------------------------===//

template <>
inline auto GreaterThan::Operation(const bool &left, const bool &right) -> bool {
  return !right && left;
}

template <>
inline auto LessThan::Operation(const bool &left, const bool &right) -> bool {
  return !left && right;
}

//===--------------------------------------------------------------------===//
// Specialized string comparison operations
//===--------------------------------------------------------------------===//

/** @brief The equality core shared by Equals and NotEquals over strings. */
struct StringComparisonOperators {
  /**
   * @brief Compare two strings for (in)equality.
   *
   * Compares through GetDataUnsafe() — the inlined prefix or the external pointer — rather
   * than the raw prefix field. The prefix is only guaranteed consistent for strings built
   * through the (data, len) constructor; comparing the actual bytes keeps this correct for
   * every string_t source, and matches BumbleString::operator==.
   *
   * @tparam INVERSE True to compute `!=` instead of `==`.
   */
  template <bool INVERSE>
  static inline auto EqualsOrNot(const string_t &a, const string_t &b) -> bool {
    const auto size = a.Size();
    if (size != b.Size()) {
      return INVERSE;
    }
    const bool equal = memcmp(a.GetDataUnsafe(), b.GetDataUnsafe(), size) == 0;
    return INVERSE ? !equal : equal;
  }
};

template <>
inline auto Equals::Operation(const string_t &left, const string_t &right) -> bool {
  return StringComparisonOperators::EqualsOrNot<false>(left, right);
}

template <>
inline auto NotEquals::Operation(const string_t &left, const string_t &right) -> bool {
  return StringComparisonOperators::EqualsOrNot<true>(left, right);
}

/** @brief Order two strings: compare the shared prefix, and break a tie on the lengths. */
template <class OP>
static auto TemplatedStringCompareOp(const string_t &left, const string_t &right) -> bool {
  auto memcmp_res = memcmp(left.GetDataUnsafe(), right.GetDataUnsafe(), std::min(left.Size(), right.Size()));
  return memcmp_res == 0 ? OP::Operation(left.Size(), right.Size()) : OP::Operation(memcmp_res, 0);
}

template <>
inline auto GreaterThan::Operation(const string_t &left, const string_t &right) -> bool {
  return TemplatedStringCompareOp<GreaterThan>(left, right);
}

template <>
inline auto GreaterThanEquals::Operation(const string_t &left, const string_t &right) -> bool {
  return TemplatedStringCompareOp<GreaterThanEquals>(left, right);
}

template <>
inline auto LessThan::Operation(const string_t &left, const string_t &right) -> bool {
  return TemplatedStringCompareOp<LessThan>(left, right);
}

template <>
inline auto LessThanEquals::Operation(const string_t &left, const string_t &right) -> bool {
  return TemplatedStringCompareOp<LessThanEquals>(left, right);
}

}  // namespace bumblebee
