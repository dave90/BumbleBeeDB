//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// numeric_utils.h
//
// Identification: src/include/common/numeric_utils.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

#include "common/config.h"
#include "common/helper.h"
#include "type/bumble_string.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * Fast rendering of a number into digits.
 *
 * The formatting runs once per row in a projection, so it is written to be cheap: two
 * digits at a time out of a lookup table, straight into memory the caller already owns
 * (a string vector's heap), with no allocation and no std::to_string.
 */
class NumericHelper {
 public:
  /** 10^i, for i in [0, 19). */
  static const int64_t POWERS_OF_TEN[20];
  /** 10.0^i, for i in [0, 40). */
  static const double DOUBLE_POWERS_OF_TEN[40];
  /** The 100 two-digit strings, concatenated: DIGITS[2i], DIGITS[2i+1] are the digits of i. */
  static const char DIGITS[];

  /** @return The number of digits of an unsigned value. */
  template <class T>
  static auto UnsignedLength(T value) -> int;

  /** @return The number of characters of a signed value, minus sign included. */
  template <class SIGNED, class UNSIGNED>
  static auto SignedLength(SIGNED value) -> int {
    int sign = -static_cast<int>(value < 0);
    UNSIGNED unsigned_value = (value ^ sign) - sign;
    return UnsignedLength(unsigned_value) - sign;
  }

  /**
   * @brief Write the digits of `value` BACKWARDS from `ptr`.
   *
   * @param value The value to render.
   * @param ptr One past the last byte to write.
   * @return char* The first byte written.
   */
  template <class T>
  static auto FormatUnsigned(T value, char *ptr) -> char * {
    while (value >= 100) {
      // Integer division is slow, so do it once per two digits instead of once per digit.
      // The trick comes from Alexandrescu's "Three Optimization Tips for C++".
      auto index = static_cast<unsigned>((value % 100) * 2);
      value /= 100;
      *--ptr = DIGITS[index + 1];
      *--ptr = DIGITS[index];
    }
    if (value < 10) {
      *--ptr = static_cast<char>('0' + value);
      return ptr;
    }
    auto index = static_cast<unsigned>(value * 2);
    *--ptr = DIGITS[index + 1];
    *--ptr = DIGITS[index];
    return ptr;
  }

  /**
   * @brief Render `value` straight into the string heap of `vector`.
   *
   * @param value The value to render.
   * @param vector The STRING vector whose heap holds the bytes.
   * @return string_t The rendered string.
   */
  template <class SIGNED, class UNSIGNED>
  static auto FormatSigned(SIGNED value, Vector &vector) -> string_t {
    int sign = -static_cast<int>(value < 0);
    UNSIGNED unsigned_value = UNSIGNED(value ^ sign) - sign;
    int length = UnsignedLength<UNSIGNED>(unsigned_value) - sign;
    string_t result = StringVector::EmptyString(vector, length);
    auto *dataptr = result.GetDataWriteable();
    auto *endptr = dataptr + length;
    endptr = FormatUnsigned(unsigned_value, endptr);
    if (sign != 0) {
      *--endptr = '-';
    }
    dataptr[length] = '\0';
    return result;
  }
};

template <>
auto NumericHelper::UnsignedLength(uint8_t value) -> int;
template <>
auto NumericHelper::UnsignedLength(uint16_t value) -> int;
template <>
auto NumericHelper::UnsignedLength(uint32_t value) -> int;
template <>
auto NumericHelper::UnsignedLength(uint64_t value) -> int;

/** Rendering of a DECIMAL: the backing integer, with a point `scale` digits from the right. */
struct DecimalToString {
  /** @return The number of characters `value` renders to at the given scale. */
  template <class SIGNED, class UNSIGNED>
  static auto DecimalLength(SIGNED value, uint8_t scale) -> int {
    if (scale == 0) {
      return NumericHelper::SignedLength<SIGNED, UNSIGNED>(value);
    }
    // The length is the larger of:
    //  - scale + 2, when the number is in (-1, 1) and renders as "0.XXX";
    //  - the integer length + 1, the extra character being the '.'.
    return MaxValue(scale + 2 + (value < 0 ? 1 : 0), NumericHelper::SignedLength<SIGNED, UNSIGNED>(value) + 1);
  }

  /**
   * @brief Render `value` at the given scale into `len` bytes at `dst`.
   *
   * @param value The backing integer.
   * @param scale The number of digits after the point.
   * @param dst The buffer to write, of exactly DecimalLength() bytes.
   * @param len The number of bytes to write.
   */
  template <class SIGNED, class UNSIGNED>
  static void FormatDecimal(SIGNED value, uint8_t scale, char *dst, idx_t len) {
    char *end = dst + len;
    if (value < 0) {
      value = -value;
      *dst = '-';
    }
    if (scale == 0) {
      NumericHelper::FormatUnsigned<UNSIGNED>(value, end);
      return;
    }
    // Write the digits after the point, then the point, then the digits before it.
    UNSIGNED minor = value % static_cast<UNSIGNED>(NumericHelper::POWERS_OF_TEN[scale]);
    UNSIGNED major = value / static_cast<UNSIGNED>(NumericHelper::POWERS_OF_TEN[scale]);
    dst = NumericHelper::FormatUnsigned<UNSIGNED>(minor, end);
    // Pad the fractional part with zeros, then add the point.
    while (dst > (end - scale)) {
      *--dst = '0';
    }
    *--dst = '.';
    NumericHelper::FormatUnsigned<UNSIGNED>(major, dst);
  }

  /** @brief Render `value` straight into the string heap of `vector`. */
  template <class SIGNED, class UNSIGNED>
  static auto Format(SIGNED value, uint8_t scale, Vector &vector) -> string_t {
    int len = DecimalLength<SIGNED, UNSIGNED>(value, scale);
    string_t result = StringVector::EmptyString(vector, len);
    FormatDecimal<SIGNED, UNSIGNED>(value, scale, result.GetDataWriteable(), len);
    return result;
  }
};

}  // namespace bumblebee
