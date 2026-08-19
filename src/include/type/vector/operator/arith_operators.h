//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// arith_operators.h
//
// Identification: src/include/type/vector/operator/arith_operators.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <type_traits>

#include "common/limits.h"
#include "type/bumble_string.h"

namespace bumblebee {

//===--------------------------------------------------------------------===//
// Arithmetic operations
//
// A functor, not a function: the execution templates take it as a type parameter so the
// per-row call inlines away and the loop stays branch-free.
//
// SIGNED OVERFLOW. `left + right` on a signed integer is undefined on overflow, and UBSan
// caught all three of +, * and - doing exactly that on the existing test corpus
// (INT_MIN + INT_MIN, INT_MIN * 5, INT_MIN - 5). The engine already depended on these wrapping,
// silently, because that is what the hardware does — but the optimiser is entitled to assume
// overflow cannot happen, so the code was one inlining decision away from misbehaving.
//
// Doing the arithmetic in the matching unsigned type and converting back is fully defined in
// C++20 (two's complement is mandated, and narrowing conversion to a signed type is modular).
// It produces bit-for-bit the same result as the wraparound relied on before, and lowers to the
// identical instruction — the cast is a no-op at the machine level.
//
// `if constexpr` rather than a helper so the discarded branch is never instantiated: these
// templates are also instantiated for float, double and string_t, where make_unsigned_t is
// ill-formed.
//===--------------------------------------------------------------------===//

/** @brief True when `T` is a signed integer, whose overflow would otherwise be undefined. */
template <class T>
inline constexpr bool IS_SIGNED_INTEGER = std::is_integral_v<T> && std::is_signed_v<T>;

/** @brief `left + right`, wrapping rather than overflowing for signed integers. */
struct Sum {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    if constexpr (IS_SIGNED_INTEGER<T>) {
      using U = std::make_unsigned_t<T>;
      return static_cast<T>(static_cast<U>(left) + static_cast<U>(right));
    } else {
      return left + right;
    }
  }
};

/** @brief `left / right`. */
struct Division {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left / right;
  }
};

/** @brief `left * right`, wrapping rather than overflowing for signed integers. */
struct Dot {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    if constexpr (IS_SIGNED_INTEGER<T>) {
      using U = std::make_unsigned_t<T>;
      return static_cast<T>(static_cast<U>(left) * static_cast<U>(right));
    } else {
      return left * right;
    }
  }
};

/** @brief `left - right`, wrapping rather than overflowing for signed integers. */
struct Difference {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    if constexpr (IS_SIGNED_INTEGER<T>) {
      using U = std::make_unsigned_t<T>;
      return static_cast<T>(static_cast<U>(left) - static_cast<U>(right));
    } else {
      return left - right;
    }
  }
};

/** @brief `left % right`. */
struct Modulo {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left % right;
  }
};

//===--------------------------------------------------------------------===//
// Logic operations
//===--------------------------------------------------------------------===//

/** @brief Bitwise `left & right`. */
struct And {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left & right;
  }
};

//===--------------------------------------------------------------------===//
// Integer divide-by-zero guards
//
// `left / right` and `left % right` are undefined behavior — a SIGFPE trap on x86 — when
// the integer divisor is zero. The floating-point paths are fine (they yield inf / NaN),
// and the DECIMAL kernels in vector_arith.cpp already saturate a zero divisor to the type
// maximum. These specializations bring the plain integer types in line with that DECIMAL
// policy: a zero divisor saturates to the type maximum rather than trapping.
//
// NOTE: the signed INT_MIN / -1 overflow is a separate edge that also traps; it is not
// guarded here, only the reported divide-by-zero.
//===--------------------------------------------------------------------===//

#define BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(TYPE)                        \
  template <>                                                          \
  inline auto Division::Operation(TYPE left, TYPE right) -> TYPE {     \
    return right == 0 ? NumericLimits<TYPE>::Maximum() : left / right; \
  }                                                                    \
  template <>                                                          \
  inline auto Modulo::Operation(TYPE left, TYPE right) -> TYPE {       \
    return right == 0 ? NumericLimits<TYPE>::Maximum() : left % right; \
  }

BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(int8_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(int16_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(int32_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(int64_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(uint8_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(uint16_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(uint32_t)
BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD(uint64_t)

#undef BUMBLEBEE_DEFINE_INT_DIVMOD_GUARD

//===--------------------------------------------------------------------===//
// Float and double modulo
//===--------------------------------------------------------------------===//

template <>
inline auto Modulo::Operation(float left, float right) -> float {
  return std::fmod(left, right);
}

template <>
inline auto Modulo::Operation(double left, double right) -> double {
  return std::fmod(left, right);
}

//===--------------------------------------------------------------------===//
// String operations
//
// Arithmetic over strings is not defined. The specializations exist only so that the
// type-dispatch switch in the kernels instantiates: a STRING never reaches them at run
// time, because the kernel rejects the type first.
//===--------------------------------------------------------------------===//

template <>
inline auto Sum::Operation(string_t left, string_t right) -> string_t {
  (void)right;
  return left;
}

template <>
inline auto Division::Operation(string_t left, string_t right) -> string_t {
  (void)right;
  return left;
}

template <>
inline auto Dot::Operation(string_t left, string_t right) -> string_t {
  (void)right;
  return left;
}

template <>
inline auto Difference::Operation(string_t left, string_t right) -> string_t {
  (void)right;
  return left;
}

template <>
inline auto Modulo::Operation(string_t left, string_t right) -> string_t {
  (void)right;
  return left;
}

template <>
inline auto And::Operation(string_t left, string_t right) -> string_t {
  (void)right;
  return left;
}

// Bitwise AND is not defined on the floating-point types either; same rationale.
template <>
inline auto And::Operation(float left, float right) -> float {
  (void)right;
  return left;
}

template <>
inline auto And::Operation(double left, double right) -> double {
  (void)right;
  return left;
}

}  // namespace bumblebee
