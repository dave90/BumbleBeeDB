//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// timestamp.h
//
// Identification: src/include/type/timestamp.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

#include "common/config.h"

namespace bumblebee {

/** A timestamp broken out into its calendar fields. */
struct TimestampStruct {
  int32_t year_;
  int8_t month_;
  int8_t day_;
  int8_t hour_;
  int8_t min_;
  int8_t sec_;
  int16_t msec_;
};

/**
 * The TIMESTAMP helpers.
 *
 * A timestamp_t is a count of MICROseconds since the epoch. Everything here is a static
 * conversion between that count and the units a user thinks in; nothing holds state.
 */
class Timestamp {
 public:
  static constexpr int32_t DAYS_PER_WEEK = 7;
  static constexpr int64_t DAYS_PER_MONTH = 30;

  static constexpr int64_t MSECS_PER_SEC = 1000;
  static constexpr int32_t SECS_PER_MINUTE = 60;
  static constexpr int32_t MINS_PER_HOUR = 60;
  static constexpr int32_t HOURS_PER_DAY = 24;
  static constexpr int32_t SECS_PER_HOUR = SECS_PER_MINUTE * MINS_PER_HOUR;
  static constexpr int32_t SECS_PER_DAY = SECS_PER_HOUR * HOURS_PER_DAY;
  static constexpr int32_t SECS_PER_WEEK = SECS_PER_DAY * DAYS_PER_WEEK;

  static constexpr int64_t MICROS_PER_MSEC = 1000;
  static constexpr int64_t MICROS_PER_SEC = MICROS_PER_MSEC * MSECS_PER_SEC;
  static constexpr int64_t MICROS_PER_MINUTE = MICROS_PER_SEC * SECS_PER_MINUTE;
  static constexpr int64_t MICROS_PER_HOUR = MICROS_PER_MINUTE * MINS_PER_HOUR;
  static constexpr int64_t MICROS_PER_DAY = MICROS_PER_HOUR * HOURS_PER_DAY;
  static constexpr int64_t MICROS_PER_WEEK = MICROS_PER_DAY * DAYS_PER_WEEK;
  static constexpr int64_t MICROS_PER_MONTH = MICROS_PER_DAY * DAYS_PER_MONTH;

  /** @return The timestamp `ms` milliseconds after the epoch. */
  static auto FromEpochMs(int64_t ms) -> timestamp_t;

  /** @return The timestamp `ns` nanoseconds after the epoch, truncated to microseconds. */
  static auto FromEpochNano(int64_t ns) -> timestamp_t;

  /** @return The timestamp `micros` microseconds after the epoch. */
  static auto FromEpochMicroSeconds(int64_t micros) -> timestamp_t;

  /** @return The microseconds elapsed since midnight of the timestamp's day. */
  static auto GetTime(timestamp_t timestamp) -> int64_t;

  /** @return The day the timestamp falls on, as days since the epoch. */
  static auto GetDate(timestamp_t timestamp) -> date_t;

  /** @return The timestamp in nanoseconds since the epoch. */
  static auto GetEpochNanoSeconds(timestamp_t timestamp) -> int64_t;

  /** @return Midnight of `date`, as a timestamp. */
  static auto FromDatetime(date_t date) -> timestamp_t;

  /**
   * @brief Parse "YYYY-MM-DD[ HH:MM[:SS[.ffffff]]]" (a 'T' separator also works).
   *
   * @param buf The characters to parse.
   * @param len The number of characters.
   * @param result Receives the parsed timestamp.
   * @return True when the whole input parsed.
   */
  [[nodiscard]] static auto TryConvertTimestamp(const char *buf, idx_t len, timestamp_t &result) -> bool;

  /** @brief Break the microseconds-since-midnight `dtime` into hour, minute, second, micros. */
  static void Convert(int64_t dtime, int32_t &hour, int32_t &min, int32_t &sec, int32_t &micros);

  /** @brief Split `timestamp` into the day it falls on and the microseconds into that day. */
  static void Convert(timestamp_t timestamp, date_t &out_date, int64_t &out_time);

  // -- Rendering ------------------------------------------------------------

  /**
   * @brief The number of characters `time` renders to, filling `micro_buffer` on the way.
   *
   * The format is HH:MM:SS, plus `.ffffff` when there are microseconds, with any trailing
   * zeros of the fraction truncated ("900000" renders as ".9").
   *
   * @param time The four fields hour, minute, second, microsecond.
   * @param micro_buffer A 6-byte scratch buffer, filled with the fraction's digits.
   * @return idx_t The number of characters.
   */
  static auto Length(int32_t time[], char micro_buffer[]) -> idx_t;

  /** @return The number of trailing zeros of the fraction, having written it to the buffer. */
  static auto FormatMicros(uint32_t microseconds, char micro_buffer[]) -> int32_t;

  /** @brief Write the `length` characters of `time` into `data`. */
  static void Format(char *data, idx_t length, int32_t time[], char micro_buffer[]);

  /** @brief Write `value` (in [0, 99]) as exactly two digits at `ptr`. */
  static void FormatTwoDigits(char *ptr, int32_t value);
};

}  // namespace bumblebee
