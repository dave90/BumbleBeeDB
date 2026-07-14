//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// limits.h
//
// Identification: src/include/common/limits.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

#include "common/config.h"

namespace bumblebee {

/**
 * @brief The numeric range of a physical type, plus the number of decimal digits it can hold.
 *
 * The primary template is only declared: every type the engine stores is specialized below.
 */
template <class T>
struct NumericLimits {
  static auto Minimum() -> T;
  static auto Maximum() -> T;
  static auto IsSigned() -> bool;
  static auto Digits() -> idx_t;
};

template <>
struct NumericLimits<int8_t> {
  static auto Minimum() -> int8_t;
  static auto Maximum() -> int8_t;
  static auto IsSigned() -> bool { return true; }
  static auto Digits() -> idx_t { return 3; }
};

template <>
struct NumericLimits<int16_t> {
  static auto Minimum() -> int16_t;
  static auto Maximum() -> int16_t;
  static auto IsSigned() -> bool { return true; }
  static auto Digits() -> idx_t { return 5; }
};

template <>
struct NumericLimits<int32_t> {
  static auto Minimum() -> int32_t;
  static auto Maximum() -> int32_t;
  static auto IsSigned() -> bool { return true; }
  static auto Digits() -> idx_t { return 10; }
};

template <>
struct NumericLimits<int64_t> {
  static auto Minimum() -> int64_t;
  static auto Maximum() -> int64_t;
  static auto IsSigned() -> bool { return true; }
  static auto Digits() -> idx_t { return 19; }
};

template <>
struct NumericLimits<uint8_t> {
  static auto Minimum() -> uint8_t;
  static auto Maximum() -> uint8_t;
  static auto IsSigned() -> bool { return false; }
  static auto Digits() -> idx_t { return 3; }
};

template <>
struct NumericLimits<uint16_t> {
  static auto Minimum() -> uint16_t;
  static auto Maximum() -> uint16_t;
  static auto IsSigned() -> bool { return false; }
  static auto Digits() -> idx_t { return 5; }
};

template <>
struct NumericLimits<uint32_t> {
  static auto Minimum() -> uint32_t;
  static auto Maximum() -> uint32_t;
  static auto IsSigned() -> bool { return false; }
  static auto Digits() -> idx_t { return 10; }
};

template <>
struct NumericLimits<uint64_t> {
  static auto Minimum() -> uint64_t;
  static auto Maximum() -> uint64_t;
  static auto IsSigned() -> bool { return false; }
  static auto Digits() -> idx_t { return 20; }
};

template <>
struct NumericLimits<float> {
  static auto Minimum() -> float;
  static auto Maximum() -> float;
  static auto IsSigned() -> bool { return true; }
  static auto Digits() -> idx_t { return 127; }
};

template <>
struct NumericLimits<double> {
  static auto Minimum() -> double;
  static auto Maximum() -> double;
  static auto IsSigned() -> bool { return true; }
  static auto Digits() -> idx_t { return 250; }
};

}  // namespace bumblebee
