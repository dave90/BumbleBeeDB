//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// timestamp_parse_test.cpp
//
// Identification: test/unit/type/timestamp_parse_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <gtest/gtest.h>

#include <string>

#include "type/timestamp.h"

namespace bumblebee {

static auto Parse(const std::string &s, timestamp_t &out) -> bool {
  return Timestamp::TryConvertTimestamp(s.data(), s.size(), out);
}

static auto MustParse(const std::string &s) -> timestamp_t {
  timestamp_t ts = 0;
  EXPECT_TRUE(Parse(s, ts)) << s;
  return ts;
}

TEST(TimestampParseTest, ParsesDateAndTimeForms) {
  // A bare date is midnight.
  EXPECT_EQ(MustParse("1970-01-01"), 0);
  EXPECT_EQ(MustParse("1970-01-01 00:00:01"), Timestamp::MICROS_PER_SEC);
  EXPECT_EQ(MustParse("1970-01-02"), Timestamp::MICROS_PER_DAY);
  // HH:MM without seconds; the 'T' separator; fractional seconds down to micros.
  EXPECT_EQ(MustParse("1970-01-01 01:30"), Timestamp::MICROS_PER_HOUR + 30 * Timestamp::MICROS_PER_MINUTE);
  EXPECT_EQ(MustParse("1970-01-01T00:00:02"), 2 * Timestamp::MICROS_PER_SEC);
  EXPECT_EQ(MustParse("1970-01-01 00:00:00.5"), 500000);
  EXPECT_EQ(MustParse("1970-01-01 00:00:00.000001"), 1);
  // Extra fractional digits truncate rather than fail.
  EXPECT_EQ(MustParse("1970-01-01 00:00:00.1234567"), 123456);
  // Round trip against the epoch converters.
  EXPECT_EQ(
      MustParse("2024-01-01 08:00:00"),
      Timestamp::FromDatetime(MustParse("2024-01-01") / Timestamp::MICROS_PER_DAY) + 8 * Timestamp::MICROS_PER_HOUR);
}

TEST(TimestampParseTest, RejectsMalformedInput) {
  timestamp_t ts = 0;
  EXPECT_FALSE(Parse("", ts));
  EXPECT_FALSE(Parse("garbage", ts));
  EXPECT_FALSE(Parse("2024-01-01 25:00:00", ts));    // hour out of range
  EXPECT_FALSE(Parse("2024-01-01 10:61:00", ts));    // minute out of range
  EXPECT_FALSE(Parse("2024-01-01 10:00:61", ts));    // second out of range
  EXPECT_FALSE(Parse("2024-01-01 10", ts));          // truncated time
  EXPECT_FALSE(Parse("2024-01-01 10:00:00 x", ts));  // trailing junk
  EXPECT_FALSE(Parse("2024-01-01 10:00:00.", ts));   // dot with no digits
}

}  // namespace bumblebee
