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

#include "common/limits.h"
#include "type/bumble_string.h"

namespace bumblebee {

//===--------------------------------------------------------------------===//
// Arithmetic operations
//
// A functor, not a function: the execution templates take it as a type parameter so the
// per-row call inlines away and the loop stays branch-free.
//===--------------------------------------------------------------------===//

/** @brief `left + right`. */
struct Sum {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left + right;
  }
};

/** @brief `left / right`. */
struct Division {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left / right;
  }
};

/** @brief `left * right`. */
struct Dot {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left * right;
  }
};

/** @brief `left - right`. */
struct Difference {
  template <class T>
  static inline auto Operation(T left, T right) -> T {
    return left - right;
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
