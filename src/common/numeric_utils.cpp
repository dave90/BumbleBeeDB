//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// numeric_utils.cpp
//
// Identification: src/common/numeric_utils.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/numeric_utils.h"

namespace bumblebee {

const char NumericHelper::DIGITS[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

const int64_t NumericHelper::POWERS_OF_TEN[]{1,
                                             10,
                                             100,
                                             1000,
                                             10000,
                                             100000,
                                             1000000,
                                             10000000,
                                             100000000,
                                             1000000000,
                                             10000000000,
                                             100000000000,
                                             1000000000000,
                                             10000000000000,
                                             100000000000000,
                                             1000000000000000,
                                             10000000000000000,
                                             100000000000000000,
                                             1000000000000000000};

const double NumericHelper::DOUBLE_POWERS_OF_TEN[]{1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                                                   1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
                                                   1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29,
                                                   1e30, 1e31, 1e32, 1e33, 1e34, 1e35, 1e36, 1e37, 1e38, 1e39};

template <>
auto NumericHelper::UnsignedLength(uint8_t value) -> int {
  int length = 1;
  length += static_cast<int>(value >= 10);
  length += static_cast<int>(value >= 100);
  return length;
}

template <>
auto NumericHelper::UnsignedLength(uint16_t value) -> int {
  int length = 1;
  length += static_cast<int>(value >= 10);
  length += static_cast<int>(value >= 100);
  length += static_cast<int>(value >= 1000);
  length += static_cast<int>(value >= 10000);
  return length;
}

template <>
auto NumericHelper::UnsignedLength(uint32_t value) -> int {
  if (value >= 10000) {
    int length = 5;
    length += static_cast<int>(value >= 100000);
    length += static_cast<int>(value >= 1000000);
    length += static_cast<int>(value >= 10000000);
    length += static_cast<int>(value >= 100000000);
    length += static_cast<int>(value >= 1000000000);
    return length;
  }
  int length = 1;
  length += static_cast<int>(value >= 10);
  length += static_cast<int>(value >= 100);
  length += static_cast<int>(value >= 1000);
  return length;
}

template <>
auto NumericHelper::UnsignedLength(uint64_t value) -> int {
  if (value >= 10000000000ULL) {
    if (value >= 1000000000000000ULL) {
      int length = 16;
      length += static_cast<int>(value >= 10000000000000000ULL);
      length += static_cast<int>(value >= 100000000000000000ULL);
      length += static_cast<int>(value >= 1000000000000000000ULL);
      length += static_cast<int>(value >= 10000000000000000000ULL);
      return length;
    }
    int length = 11;
    length += static_cast<int>(value >= 100000000000ULL);
    length += static_cast<int>(value >= 1000000000000ULL);
    length += static_cast<int>(value >= 10000000000000ULL);
    length += static_cast<int>(value >= 100000000000000ULL);
    return length;
  }
  if (value >= 100000ULL) {
    int length = 6;
    length += static_cast<int>(value >= 1000000ULL);
    length += static_cast<int>(value >= 10000000ULL);
    length += static_cast<int>(value >= 100000000ULL);
    length += static_cast<int>(value >= 1000000000ULL);
    return length;
  }
  int length = 1;
  length += static_cast<int>(value >= 10ULL);
  length += static_cast<int>(value >= 100ULL);
  length += static_cast<int>(value >= 1000ULL);
  length += static_cast<int>(value >= 10000ULL);
  return length;
}

}  // namespace bumblebee
