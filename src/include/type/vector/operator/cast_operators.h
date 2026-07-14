//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// cast_operators.h
//
// Identification: src/include/type/vector/operator/cast_operators.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

#include "common/config.h"
#include "common/exception.h"
#include "common/limits.h"
#include "common/numeric_utils.h"
#include "common/util/string_util.h"
#include "type/bumble_string.h"
#include "type/date.h"
#include "type/timestamp.h"
#include "type/vector/vector.h"

namespace bumblebee {

//===--------------------------------------------------------------------===//
// Numeric -> Numeric
//===--------------------------------------------------------------------===//

/** @brief The unchecked numeric cast: a plain static_cast, wrapping on overflow. */
struct Cast {
  /**
   * @brief Cast `input` from SRC to DST.
   *
   * @tparam SRC The source type.
   * @tparam DST The target type.
   */
  template <class SRC, class DST>
  static inline auto Operation(SRC input) -> DST {
    if constexpr (std::is_arithmetic_v<SRC> && std::is_arithmetic_v<DST>) {
      return static_cast<DST>(input);
    } else {
      // A non-arithmetic type (e.g. a string) lands here.
      throw NotImplementedException("Cast: unsupported type for a numeric cast");
    }
  }
};

/**
 * @brief The checked numeric cast: reports failure instead of wrapping.
 *
 * Returns false when `value` does not fit in DST, so the caller can turn the row NULL and
 * raise a conversion error rather than silently storing a wrapped value.
 */
struct NumericTryCast {
  template <class SRC, class DST>
  static inline auto Operation(SRC &value, DST &result) -> bool {
    if (NumericLimits<SRC>::IsSigned() != NumericLimits<DST>::IsSigned()) {
      if (NumericLimits<SRC>::IsSigned()) {
        // Signed -> unsigned.
        if (NumericLimits<SRC>::Digits() > NumericLimits<DST>::Digits()) {
          if (value < 0 || value > static_cast<SRC>(NumericLimits<DST>::Maximum())) {
            return false;
          }
        } else {
          if (value < 0) {
            return false;
          }
        }
        result = static_cast<DST>(value);
        return true;
      }
      // Unsigned -> signed.
      if (NumericLimits<SRC>::Digits() >= NumericLimits<DST>::Digits()) {
        if (value <= static_cast<SRC>(NumericLimits<DST>::Maximum())) {
          result = static_cast<DST>(value);
          return true;
        }
        return false;
      }
      result = static_cast<DST>(value);
      return true;
    }
    // Same signedness.
    if (NumericLimits<DST>::Digits() >= NumericLimits<SRC>::Digits()) {
      result = static_cast<DST>(value);
      return true;
    }
    if (value < static_cast<SRC>(NumericLimits<DST>::Minimum()) ||
        value > static_cast<SRC>(NumericLimits<DST>::Maximum())) {
      return false;
    }
    result = static_cast<DST>(value);
    return true;
  }
};

//===--------------------------------------------------------------------===//
// String -> Numeric
//===--------------------------------------------------------------------===//

/** @brief The per-character policy the integer parse loop drives. */
struct IntegerCastOperation {
  /** @brief Fold one more digit into `result`, reporting overflow. */
  template <class T, bool NEGATIVE>
  static auto HandleDigit(T &result, uint8_t digit) -> bool {
    if (NEGATIVE) {
      if (result < (NumericLimits<T>::Minimum() + digit) / 10) {
        return false;
      }
      result = result * 10 - digit;
    } else {
      if (result > (NumericLimits<T>::Maximum() - digit) / 10) {
        return false;
      }
      result = result * 10 + digit;
    }
    return true;
  }

  /** @brief Apply a trailing `e<exponent>`, reporting overflow. */
  template <class T, bool NEGATIVE>
  static auto HandleExponent(T &result, int64_t exponent) -> bool {
    double dbl_res = static_cast<double>(result) * std::pow(10.0L, static_cast<long double>(exponent));
    if (dbl_res < static_cast<double>(NumericLimits<T>::Minimum()) ||
        dbl_res > static_cast<double>(NumericLimits<T>::Maximum())) {
      return false;
    }
    result = static_cast<T>(dbl_res);
    return true;
  }

  template <class T, bool NEGATIVE>
  static auto HandleDecimal(T &result, uint8_t digit) -> bool {
    (void)result;
    (void)digit;
    return true;
  }

  template <class T>
  static auto Finalize(T &result) -> bool {
    (void)result;
    return true;
  }
};

/**
 * @brief Parse `len` bytes of `buf` as an integer into `result`.
 *
 * @tparam NEGATIVE Whether a leading '-' was already consumed.
 * @tparam ALLOW_EXPONENT Whether a trailing `e<exp>` is accepted.
 * @return True on a full, in-range parse.
 */
template <class T, bool NEGATIVE, bool ALLOW_EXPONENT, class OP = IntegerCastOperation>
static auto IntegerCastLoop(const char *buf, idx_t len, T &result) -> bool {
  idx_t start_pos = (NEGATIVE || *buf == '+') ? 1 : 0;
  idx_t pos = start_pos;
  while (pos < len) {
    if (!StringUtil::CharacterIsDigit(buf[pos])) {
      if (buf[pos] == '.') {
        // A decimal point: this is not an integer. (The original could optionally have
        // truncated the fractional part; it rejects instead, and so do we.)
        return false;
      }
      if (StringUtil::CharacterIsSpace(buf[pos])) {
        // Skip trailing spaces; anything else after them is a parse error.
        while (++pos < len) {
          if (!StringUtil::CharacterIsSpace(buf[pos])) {
            return false;
          }
        }
        break;
      }
      if (ALLOW_EXPONENT) {
        if (buf[pos] == 'e' || buf[pos] == 'E') {
          if (pos == start_pos) {
            return false;
          }
          pos++;
          if (pos >= len) {
            return false;
          }
          int64_t exponent = 0;
          bool negative = buf[pos] == '-';
          if (negative) {
            if (!IntegerCastLoop<int64_t, true, false>(buf + pos, len - pos, exponent)) {
              return false;
            }
          } else {
            if (!IntegerCastLoop<int64_t, false, false>(buf + pos, len - pos, exponent)) {
              return false;
            }
          }
          return OP::template HandleExponent<T, NEGATIVE>(result, exponent);
        }
      }
      return false;
    }
    auto digit = static_cast<uint8_t>(buf[pos++] - '0');
    if (!OP::template HandleDigit<T, NEGATIVE>(result, digit)) {
      return false;
    }
  }
  if (!OP::template Finalize<T>(result)) {
    return false;
  }
  return pos > start_pos;
}

/** @brief Parse a string as an integer. Reports failure instead of throwing. */
struct TryIntegerCast {
  /** @brief Parse the whole of `val`. */
  template <class INPUT_TYPE, class T, bool IS_SIGNED = true, bool ALLOW_EXPONENT = true,
            class OP = IntegerCastOperation, bool ZERO_INITIALIZE = true>
  static inline auto Operation(INPUT_TYPE &val, T &result) -> bool {
    const char *buf = val.GetDataUnsafe();
    idx_t len = val.Length();
    return TryIntegerCast::Operation<INPUT_TYPE, T, IS_SIGNED, ALLOW_EXPONENT, OP, ZERO_INITIALIZE>(buf, len, result);
  }

  /** @brief Parse `len` bytes of `buf`. */
  template <class INPUT_TYPE, class T, bool IS_SIGNED = true, bool ALLOW_EXPONENT = true,
            class OP = IntegerCastOperation, bool ZERO_INITIALIZE = true>
  static inline auto Operation(const char *buf, idx_t len, T &result) -> bool {
    // Skip any leading spaces.
    while (len > 0 && StringUtil::CharacterIsSpace(*buf)) {
      buf++;
      len--;
    }
    if (len == 0) {
      return false;
    }
    bool negative = *buf == '-';

    if (ZERO_INITIALIZE) {
      memset(&result, 0, sizeof(T));
    }
    if (!negative) {
      return IntegerCastLoop<T, false, ALLOW_EXPONENT, OP>(buf, len, result);
    }
    if (!IS_SIGNED) {
      // An unsigned type only accepts a leading '-' if the value is -0.
      idx_t pos = 1;
      while (pos < len) {
        if (buf[pos++] != '0') {
          return false;
        }
      }
    }
    return IntegerCastLoop<T, true, ALLOW_EXPONENT, OP>(buf, len, result);
  }
};

/** @brief Fold the accumulated fractional digits into `result`. */
template <class T, bool NEGATIVE>
static void ComputeDoubleResult(T &result, idx_t decimal, idx_t decimal_factor) {
  if (decimal_factor > 1) {
    if (NEGATIVE) {
      result -= static_cast<T>(decimal) / static_cast<T>(decimal_factor);
    } else {
      result += static_cast<T>(decimal) / static_cast<T>(decimal_factor);
    }
  }
}

/** @brief Parse `len` bytes of `buf` as a floating-point value into `result`. */
template <class T, bool NEGATIVE>
static auto DoubleCastLoop(const char *buf, idx_t len, T &result) -> bool {
  idx_t start_pos = (NEGATIVE || *buf == '+') ? 1 : 0;
  idx_t pos = start_pos;
  idx_t decimal = 0;
  idx_t decimal_factor = 0;
  while (pos < len) {
    if (!StringUtil::CharacterIsDigit(buf[pos])) {
      if (buf[pos] == '.') {
        if (decimal_factor != 0) {
          // A second decimal point.
          return false;
        }
        decimal_factor = 1;
        pos++;
        continue;
      }
      if (StringUtil::CharacterIsSpace(buf[pos])) {
        // Skip trailing spaces; anything else after them is a parse error.
        while (++pos < len) {
          if (!StringUtil::CharacterIsSpace(buf[pos])) {
            return false;
          }
        }
        ComputeDoubleResult<T, NEGATIVE>(result, decimal, decimal_factor);
        return true;
      }
      if (buf[pos] == 'e' || buf[pos] == 'E') {
        if (pos == start_pos) {
          return false;
        }
        // An exponent: parse an integer, this time without allowing another exponent.
        pos++;
        int64_t exponent = 0;
        if (!TryIntegerCast::Operation<string_t, int64_t, true, false>(buf + pos, len - pos, exponent)) {
          return false;
        }
        ComputeDoubleResult<T, NEGATIVE>(result, decimal, decimal_factor);
        if (result > NumericLimits<T>::Maximum() / static_cast<T>(std::pow(10.0L, static_cast<long double>(exponent)))) {
          return false;
        }
        result = result * static_cast<T>(std::pow(10.0L, static_cast<long double>(exponent)));
        return true;
      }
      return false;
    }
    T digit = static_cast<T>(buf[pos++] - '0');
    if (decimal_factor == 0) {
      result = result * 10 + (NEGATIVE ? -digit : digit);
    } else {
      if (decimal_factor >= 1000000000000000000ULL) {
        // The fractional part would overflow: ignore any further digits.
        continue;
      }
      decimal = decimal * 10 + static_cast<idx_t>(digit);
      decimal_factor *= 10;
    }
  }
  ComputeDoubleResult<T, NEGATIVE>(result, decimal, decimal_factor);
  return pos > start_pos;
}

/** @return True if `value` is a finite number. */
template <class T>
auto CheckDoubleValidity(T value) -> bool;

// NOTE: these must be `inline`. An explicit specialization of a function template is NOT
// implicitly inline, so the original's non-inline definitions in a header were a latent
// ODR violation the moment a second translation unit included them.
template <>
inline auto CheckDoubleValidity(float value) -> bool {
  return !(std::isnan(value) || std::isinf(value));
}

template <>
inline auto CheckDoubleValidity(double value) -> bool {
  return !(std::isnan(value) || std::isinf(value));
}

/** @brief Parse a string as a floating-point value. Reports failure instead of throwing. */
struct TryDoubleCast {
  template <class INPUT_TYPE, class T>
  static auto Operation(INPUT_TYPE &val, T &result) -> bool {
    const char *buf = val.GetDataUnsafe();
    idx_t len = val.Length();

    // Skip any leading spaces.
    while (len > 0 && StringUtil::CharacterIsSpace(*buf)) {
      buf++;
      len--;
    }
    if (len == 0) {
      return false;
    }
    bool negative = *buf == '-';

    result = 0;
    if (!negative) {
      if (!DoubleCastLoop<T, false>(buf, len, result)) {
        return false;
      }
    } else {
      if (!DoubleCastLoop<T, true>(buf, len, result)) {
        return false;
      }
    }
    return CheckDoubleValidity<T>(result);
  }
};

//===--------------------------------------------------------------------===//
// Numeric -> String
//
// The result is written straight into the string heap of the target Vector, which is why
// these take the Vector: a string_t is a view, it does not own its bytes.
//===--------------------------------------------------------------------===//

/** @brief Render a numeric value into the heap of `vector`. */
struct StringCast {
  template <class T>
  static inline auto Operation(T value, Vector &vector) -> string_t {
    (void)value;
    (void)vector;
    throw NotImplementedException("StringCast: unsupported type for a string cast");
  }
};

/**
 * @brief Render a numeric value into the heap of `vector`.
 *
 * ADAPTATION: the original short-circuited `NumericLimits<T>::maximum() == value` to the
 * empty string, treating the type's maximum as a NULL sentinel. That is wrong twice over
 * here: this engine's physical NULL fill is NullValue<T>() == minimum(), not maximum, and
 * NULL is carried by the ValidityMask anyway — UnaryExecution never calls the operator on
 * a NULL row. All the guard could still do is corrupt a legitimate maximum-valued row
 * (e.g. 65535::USMALLINT would render as ""). Dropped.
 */
struct StringTryCast {
  template <class T>
  static inline auto Operation(T value, Vector &vector) -> string_t {
    return StringCast::Operation(value, vector);
  }
};

template <>
inline auto StringCast::Operation(uint8_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<uint8_t, uint8_t>(value, vector);
}

template <>
inline auto StringCast::Operation(uint16_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<uint16_t, uint16_t>(value, vector);
}

template <>
inline auto StringCast::Operation(uint32_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<uint32_t, uint32_t>(value, vector);
}

template <>
inline auto StringCast::Operation(uint64_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<uint64_t, uint64_t>(value, vector);
}

template <>
inline auto StringCast::Operation(int8_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<int8_t, uint8_t>(value, vector);
}

template <>
inline auto StringCast::Operation(int16_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<int16_t, uint16_t>(value, vector);
}

template <>
inline auto StringCast::Operation(int32_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<int32_t, uint32_t>(value, vector);
}

template <>
inline auto StringCast::Operation(int64_t value, Vector &vector) -> string_t {
  return NumericHelper::FormatSigned<int64_t, uint64_t>(value, vector);
}

template <>
inline auto StringCast::Operation(float value, Vector &vector) -> string_t {
  std::string s = std::to_string(value);
  return StringVector::AddString(vector, s);
}

template <>
inline auto StringCast::Operation(double value, Vector &vector) -> string_t {
  std::string s = std::to_string(value);
  return StringVector::AddString(vector, s);
}

//===--------------------------------------------------------------------===//
// DECIMAL -> String
//===--------------------------------------------------------------------===//

/** @brief Render the backing integer of a DECIMAL at the given width and scale. */
struct StringCastFromDecimal {
  template <class SRC>
  static inline auto Operation(SRC input, uint8_t width, uint8_t scale, Vector &result) -> string_t {
    (void)input;
    (void)width;
    (void)scale;
    (void)result;
    throw NotImplementedException("StringCastFromDecimal: unsupported backing type");
  }
};

/** @brief See StringTryCast for why the original's maximum-as-NULL guard is gone. */
struct StringTryCastFromDecimal {
  template <class SRC>
  static inline auto Operation(SRC input, uint8_t width, uint8_t scale, Vector &result) -> string_t {
    return StringCastFromDecimal::Operation(input, width, scale, result);
  }
};

template <>
inline auto StringCastFromDecimal::Operation(int16_t input, uint8_t width, uint8_t scale, Vector &result) -> string_t {
  (void)width;
  return DecimalToString::Format<int16_t, uint16_t>(input, scale, result);
}

template <>
inline auto StringCastFromDecimal::Operation(int32_t input, uint8_t width, uint8_t scale, Vector &result) -> string_t {
  (void)width;
  return DecimalToString::Format<int32_t, uint32_t>(input, scale, result);
}

template <>
inline auto StringCastFromDecimal::Operation(int64_t input, uint8_t width, uint8_t scale, Vector &result) -> string_t {
  (void)width;
  return DecimalToString::Format<int64_t, uint64_t>(input, scale, result);
}

//===--------------------------------------------------------------------===//
// DATE / TIMESTAMP -> String
//===--------------------------------------------------------------------===//

/** @brief Render a DATE (days since the epoch) into the heap of `result`. */
struct StringCastFromDate {
  template <class SRC>
  static inline auto Operation(SRC input, Vector &result) -> string_t {
    (void)input;
    (void)result;
    throw NotImplementedException("StringCastFromDate: unsupported backing type");
  }
};

template <>
inline auto StringCastFromDate::Operation(date_t input, Vector &result) -> string_t {
  int32_t date[3];
  Date::Convert(input, date[0], date[1], date[2]);

  idx_t year_length;
  bool add_bc;
  idx_t length = Date::Length(date, year_length, add_bc);

  string_t result_string = StringVector::EmptyString(result, length);
  auto *data = result_string.GetDataWriteable();

  Date::Format(data, date, year_length, add_bc);

  return result_string;
}

/** @brief Render a TIMESTAMP (microseconds since the epoch) into the heap of `vector`. */
struct StringCastFromTimestamp {
  template <class SRC>
  static inline auto Operation(SRC input, Vector &vector) -> string_t {
    (void)input;
    (void)vector;
    throw NotImplementedException("StringCastFromTimestamp: unsupported backing type");
  }
};

template <>
inline auto StringCastFromTimestamp::Operation(timestamp_t input, Vector &vector) -> string_t {
  date_t date_entry;
  int64_t time_entry;
  Timestamp::Convert(input, date_entry, time_entry);

  int32_t date[3];
  int32_t time[4];
  Date::Convert(date_entry, date[0], date[1], date[2]);
  Timestamp::Convert(time_entry, time[0], time[1], time[2], time[3]);

  // The rendering is DATE, a space, then TIME.
  idx_t year_length;
  bool add_bc;
  char micro_buffer[6];
  idx_t date_length = Date::Length(date, year_length, add_bc);
  idx_t time_length = Timestamp::Length(time, micro_buffer);
  idx_t length = date_length + time_length + 1;

  string_t result = StringVector::EmptyString(vector, length);
  auto *data = result.GetDataWriteable();

  Date::Format(data, date, year_length, add_bc);
  data[date_length] = ' ';
  Timestamp::Format(data + date_length + 1, time_length, time, micro_buffer);

  return result;
}

}  // namespace bumblebee
