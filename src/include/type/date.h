//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// date.h
//
// Identification: src/include/type/date.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

#include "common/config.h"
#include "type/bumble_string.h"
#include "type/timestamp.h"

namespace bumblebee {

/**
 * The DATE helpers.
 *
 * A date_t is a count of days since 1970-01-01. Converting to and from a calendar date is
 * done against the lookup tables below rather than by arithmetic: the leap-year pattern
 * repeats every 400 years, so a date is normalized into one 400-year interval and then
 * resolved with a table lookup.
 */
class Date {
 public:
  static const string_t MONTH_NAMES[12];
  static const string_t MONTH_NAMES_ABBREVIATED[12];
  static const string_t DAY_NAMES[7];
  static const string_t DAY_NAMES_ABBREVIATED[7];
  static const int32_t NORMAL_DAYS[13];
  static const int32_t CUMULATIVE_DAYS[13];
  static const int32_t LEAP_DAYS[13];
  static const int32_t CUMULATIVE_LEAP_DAYS[13];
  static const int32_t CUMULATIVE_YEAR_DAYS[401];
  static const int8_t MONTH_PER_DAY_OF_YEAR[365];
  static const int8_t LEAP_MONTH_PER_DAY_OF_YEAR[366];

  /** The earliest representable date, 5877641-06-23 (BC), i.e. -2^31 days. */
  static constexpr int32_t DATE_MIN_YEAR = -5877641;
  static constexpr int32_t DATE_MIN_MONTH = 6;
  static constexpr int32_t DATE_MIN_DAY = 23;
  /** The latest representable date, 5881580-07-11, i.e. 2^31 days. */
  static constexpr int32_t DATE_MAX_YEAR = 5881580;
  static constexpr int32_t DATE_MAX_MONTH = 7;
  static constexpr int32_t DATE_MAX_DAY = 11;
  static constexpr int32_t EPOCH_YEAR = 1970;

  /** The leap-year pattern repeats every 400 years, which is exactly 146097 days. */
  static constexpr int32_t YEAR_INTERVAL = 400;
  static constexpr int32_t DAYS_PER_YEAR_INTERVAL = 146097;

  /** @return True if `year` is a leap year. */
  static auto IsLeapYear(int32_t year) -> bool;

  /** @return True if year-month-day is a real, representable date. */
  static auto IsValid(int32_t year, int32_t month, int32_t day) -> bool;

  /**
   * @brief Convert a calendar date into days since the epoch.
   *
   * @param year The year.
   * @param month The month, in [1, 12].
   * @param day The day of the month, 1-based.
   * @param result The days since the epoch.
   * @return bool False if the date is not valid; `result` is then untouched.
   */
  [[nodiscard]] static auto TryFromDate(int32_t year, int32_t month, int32_t day, date_t &result) -> bool;

  /** @brief Break `d` (days since the epoch) into year, month and day. */
  static void Convert(int32_t d, int32_t &year, int32_t &month, int32_t &day);

  /**
   * @brief Parse a date out of `buf`, accepting `-`, `/`, `\` or a space as the separator.
   *
   * A trailing " (BC)" is accepted. In strict mode nothing but whitespace may follow.
   *
   * @param buf The characters to parse.
   * @param len The number of characters.
   * @param pos The position parsing stopped at.
   * @param result The parsed date.
   * @param strict Whether trailing non-space characters are an error.
   * @return bool True if a date was parsed.
   */
  [[nodiscard]] static auto TryConvertDate(const char *buf, idx_t len, idx_t &pos, date_t &result, bool strict) -> bool;

  /** @brief Parse one or two digits at `pos`. */
  static auto ParseDoubleDigit(const char *buf, idx_t len, idx_t &pos, int32_t &result) -> bool;

  /** @return The seconds since the epoch of midnight on `date`. */
  static auto Epoch(date_t date) -> int64_t;

  /** @return The nanoseconds since the epoch of midnight on `date`. */
  static auto EpochNanoseconds(date_t date) -> int64_t;

  // -- Rendering ------------------------------------------------------------

  /**
   * @brief The number of characters the date renders to: YYYY-MM-DD, plus " (BC)" if needed.
   *
   * NOTE: this MUTATES `date[0]` for a BC year, flipping it to its positive rendering.
   * Format() relies on that having happened.
   *
   * @param date The three fields year, month, day.
   * @param year_length The number of digits the year renders to.
   * @param add_bc Whether the " (BC)" suffix is needed.
   * @return idx_t The number of characters.
   */
  static auto Length(int32_t date[], idx_t &year_length, bool &add_bc) -> idx_t;

  /** @brief Write the date into `data`, which must hold the count Length() returned. */
  static void Format(char *data, int32_t date[], idx_t year_length, bool add_bc);

 private:
  /** @brief Normalize `n` into one 400-year interval, yielding the year and its offset. */
  static void ExtractYearOffset(int32_t &n, int32_t &year, int32_t &year_offset);
};

}  // namespace bumblebee
