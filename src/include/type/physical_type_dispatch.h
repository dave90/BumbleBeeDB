//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_type_dispatch.h
//
// Identification: src/include/type/physical_type_dispatch.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <utility>

#include "type/bumble_string.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * The one place the "switch over every numeric PhysicalType, call `F<T>`" ladder is spelled out.
 *
 * Vectorized kernels are templates over the element type, so every call site used to repeat the
 * same ten-case switch — 413 `case PhysicalType::` labels across 18 files, all the same shape.
 * These helpers state that ladder once; a call site passes a generic lambda and gets
 * `fn.template operator()<T>()` for the matching element type, exactly like the proven
 * `DispatchNullsAndSelection` pattern in the aggregate kernels.
 *
 * `otherwise` runs for any type outside the set — a call site keeps its own fallback (a string
 * kernel, a nested-type path, or its own error message) without a second switch. Both callables
 * must agree on the return type; the compiler deduces it, so value-returning kernels work
 * unchanged. The lambdas inline away entirely (same instantiations as the hand-written switch).
 */

/** @brief Invoke `fn.template operator()<T>()` for the ten numeric physical types, or
 * `otherwise()` for anything else. */
template <class FN, class ELSE_FN>
[[gnu::always_inline]] inline auto DispatchNumericPhysicalType(PhysicalType type, FN &&fn, ELSE_FN &&otherwise) {
  switch (type) {
    case PhysicalType::TINYINT:
      return fn.template operator()<int8_t>();
    case PhysicalType::SMALLINT:
      return fn.template operator()<int16_t>();
    case PhysicalType::INTEGER:
      return fn.template operator()<int32_t>();
    case PhysicalType::BIGINT:
      return fn.template operator()<int64_t>();
    case PhysicalType::UTINYINT:
      return fn.template operator()<uint8_t>();
    case PhysicalType::USMALLINT:
      return fn.template operator()<uint16_t>();
    case PhysicalType::UINTEGER:
      return fn.template operator()<uint32_t>();
    case PhysicalType::UBIGINT:
      return fn.template operator()<uint64_t>();
    case PhysicalType::FLOAT:
      return fn.template operator()<float>();
    case PhysicalType::DOUBLE:
      return fn.template operator()<double>();
    default:
      return otherwise();
  }
}

/** @brief Like `DispatchNumericPhysicalType`, with STRING (`string_t`) in the dispatched set —
 * the shape of kernels whose string instantiation is the same template as the numeric ones. */
template <class FN, class ELSE_FN>
[[gnu::always_inline]] inline auto DispatchNumericAndStringPhysicalType(PhysicalType type, FN &&fn,
                                                                        ELSE_FN &&otherwise) {
  if (type == PhysicalType::STRING) {
    return fn.template operator()<string_t>();
  }
  return DispatchNumericPhysicalType(type, std::forward<FN>(fn), std::forward<ELSE_FN>(otherwise));
}

}  // namespace bumblebee
