//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// timestamp.cpp
//
// Identification: src/type/timestamp.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/timestamp.h"

#include <cstring>

#include "common/macros.h"
#include "common/util/string_util.h"
#include "type/date.h"
#include "common/numeric_utils.h"

namespace bumblebee {

auto Timestamp::FromEpochMs(int64_t ms) -> timestamp_t { return ms * MICROS_PER_MSEC; }

auto Timestamp::FromEpochNano(int64_t ns) -> timestamp_t { return ns / 1000; }

auto Timestamp::FromEpochMicroSeconds(int64_t micros) -> timestamp_t { return micros; }

auto Timestamp::GetTime(timestamp_t timestamp) -> int64_t {
  auto days = Timestamp::GetDate(timestamp);
  return timestamp - (static_cast<int64_t>(days) * MICROS_PER_DAY);
}

auto Timestamp::GetDate(timestamp_t timestamp) -> date_t {
  // The (timestamp < 0) terms make the division floor rather than truncate, so that a
  // timestamp before the epoch lands on the day it falls in, not the day after.
  return static_cast<date_t>((timestamp + static_cast<int64_t>(timestamp < 0)) / MICROS_PER_DAY -
                             static_cast<int64_t>(timestamp < 0));
}

auto Timestamp::GetEpochNanoSeconds(timestamp_t timestamp) -> int64_t {
  const int64_t ns_in_us = 1000;
  return timestamp * ns_in_us;
}

auto Timestamp::FromDatetime(date_t date) -> timestamp_t { return static_cast<int64_t>(date) * MICROS_PER_DAY; }

void Timestamp::Convert(int64_t dtime, int32_t &hour, int32_t &min, int32_t &sec, int32_t &micros) {
  int64_t time = dtime;
  hour = static_cast<int32_t>(time / MICROS_PER_HOUR);
  time -= static_cast<int64_t>(hour) * MICROS_PER_HOUR;
  min = static_cast<int32_t>(time / MICROS_PER_MINUTE);
  time -= static_cast<int64_t>(min) * MICROS_PER_MINUTE;
  sec = static_cast<int32_t>(time / MICROS_PER_SEC);
  time -= static_cast<int64_t>(sec) * MICROS_PER_SEC;
  micros = static_cast<int32_t>(time);
}

void Timestamp::Convert(timestamp_t timestamp, date_t &out_date, int64_t &out_time) {
  out_date = GetDate(timestamp);
  out_time = timestamp - (static_cast<int64_t>(out_date) * MICROS_PER_DAY);
}

auto Timestamp::Length(int32_t time[], char micro_buffer[]) -> idx_t {
  // The format is HH:MM:SS, with the microseconds appended after a '.'.
  idx_t length;
  if (time[3] == 0) {
    // No microseconds: just HH:MM:SS.
    length = 8;
  } else {
    length = 15;
    // The fraction drops its trailing zeros, so ".900000" renders as ".9".
    length -= FormatMicros(static_cast<uint32_t>(time[3]), micro_buffer);
  }
  return length;
}

auto Timestamp::FormatMicros(uint32_t microseconds, char micro_buffer[]) -> int32_t {
  char *endptr = micro_buffer + 6;
  endptr = NumericHelper::FormatUnsigned<uint32_t>(microseconds, endptr);
  // Left-pad the fraction with zeros: 9 microseconds is ".000009", not ".9".
  while (endptr > micro_buffer) {
    *--endptr = '0';
  }
  int32_t trailing_zeros = 0;
  for (idx_t i = 5; i > 0; i--) {
    if (micro_buffer[i] != '0') {
      break;
    }
    trailing_zeros++;
  }
  return trailing_zeros;
}

void Timestamp::Format(char *data, idx_t length, int32_t time[], char micro_buffer[]) {
  auto *ptr = data;
  ptr[2] = ':';
  ptr[5] = ':';
  for (int i = 0; i <= 2; i++) {
    FormatTwoDigits(ptr, time[i]);
    ptr += 3;
  }
  if (length > 8) {
    // The microseconds go at the end, after the point.
    data[8] = '.';
    memcpy(data + 9, micro_buffer, length - 9);
  }
}

void Timestamp::FormatTwoDigits(char *ptr, int32_t value) {
  BUMBLEBEE_ASSERT(value >= 0 && value <= 99, "Timestamp::FormatTwoDigits: the value must fit in two digits");
  if (value < 10) {
    ptr[0] = '0';
    ptr[1] = static_cast<char>('0' + value);
  } else {
    auto index = static_cast<unsigned>(value * 2);
    ptr[0] = NumericHelper::DIGITS[index];
    ptr[1] = NumericHelper::DIGITS[index + 1];
  }
}

auto Timestamp::TryConvertTimestamp(const char *buf, idx_t len, timestamp_t &result) -> bool {
  idx_t pos = 0;
  date_t date = 0;
  if (!Date::TryConvertDate(buf, len, pos, date, /*strict=*/false)) {
    return false;
  }
  result = FromDatetime(date);

  // Skip trailing whitespace; a bare date is midnight.
  auto skip_spaces = [&]() {
    while (pos < len && StringUtil::CharacterIsSpace(buf[pos])) {
      pos++;
    }
  };
  skip_spaces();
  if (pos == len) {
    return true;
  }
  // The date/time separator: one space (already consumed above) or a 'T'.
  if (buf[pos] == 'T' || buf[pos] == 't') {
    pos++;
  }

  auto parse_two_digits = [&](int32_t &out, int32_t max) -> bool {
    if (pos + 1 >= len || !StringUtil::CharacterIsDigit(buf[pos]) || !StringUtil::CharacterIsDigit(buf[pos + 1])) {
      return false;
    }
    out = (buf[pos] - '0') * 10 + (buf[pos + 1] - '0');
    pos += 2;
    return out <= max;
  };

  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  int64_t micros = 0;
  if (!parse_two_digits(hour, 23)) {
    return false;
  }
  if (pos >= len || buf[pos] != ':') {
    return false;
  }
  pos++;
  if (!parse_two_digits(minute, 59)) {
    return false;
  }
  if (pos < len && buf[pos] == ':') {
    pos++;
    if (!parse_two_digits(second, 59)) {
      return false;
    }
    if (pos < len && buf[pos] == '.') {
      pos++;
      // Up to six fractional digits, scaled to microseconds; further digits are truncated.
      int64_t scale = 100000;
      bool any = false;
      while (pos < len && StringUtil::CharacterIsDigit(buf[pos])) {
        if (scale > 0) {
          micros += (buf[pos] - '0') * scale;
          scale /= 10;
        }
        pos++;
        any = true;
      }
      if (!any) {
        return false;
      }
    }
  }
  skip_spaces();
  if (pos != len) {
    return false;
  }

  result = FromDatetime(date) + hour * MICROS_PER_HOUR + minute * MICROS_PER_MINUTE + second * MICROS_PER_SEC + micros;
  return true;
}

}  // namespace bumblebee
