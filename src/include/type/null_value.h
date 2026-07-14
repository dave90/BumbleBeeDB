//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// null_value.h
//
// Identification: src/include/type/null_value.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <limits>
#include <string>

#include "common/macros.h"
#include "type/bumble_string.h"

namespace bumblebee {

/**
 * @brief The defensive physical fill written into a NULL slot. WRITE-ONLY.
 *
 * NULL is detected via the ValidityMask, never by comparing a value to NullValue(): the
 * sentinel is a legitimate value for the unsigned and minimum-valued types. Do not use
 * this for null detection.
 */
template <class T>
inline auto NullValue() -> T {
  return std::numeric_limits<T>::min();
}

constexpr const char STR_NIL[2] = {'\200', '\0'};

template <>
inline auto NullValue() -> const char * {
  BUMBLEBEE_ASSERT(STR_NIL[0] == '\200' && STR_NIL[1] == '\0', "string null sentinel corrupted");
  return STR_NIL;
}

template <>
inline auto NullValue() -> string_t {
  return string_t(NullValue<const char *>());
}

template <>
inline auto NullValue() -> char * {
  return const_cast<char *>(NullValue<const char *>());
}

template <>
inline auto NullValue() -> std::string {
  return {NullValue<const char *>()};
}

template <>
inline auto NullValue() -> float {
  return NAN;
}

template <>
inline auto NullValue() -> double {
  return NAN;
}

}  // namespace bumblebee
