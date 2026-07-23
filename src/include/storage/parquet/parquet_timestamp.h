//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_timestamp.h
//
// Identification: src/include/storage/parquet/parquet_timestamp.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/helper.h"
#include "type/date.h"
#include "type/timestamp.h"

namespace bumblebee {

/** Parquet/Impala 96-bit timestamp: 8 bytes nanoseconds-since-midnight + 4 bytes Julian day. */
struct Int96 {
  uint32_t value[3];
};

static constexpr int64_t JULIAN_TO_UNIX_EPOCH_DAYS = 2440588LL;
static constexpr int64_t MILLISECONDS_PER_DAY = 86400000LL;
static constexpr int64_t NANOSECONDS_PER_DAY = MILLISECONDS_PER_DAY * 1000LL * 1000LL;
static constexpr int64_t SECONDS_PER_DAY = 86400LL;

/** @brief Parquet DATE payload (days since epoch) is exactly a date_t. */
inline auto ParquetIntToDate(const int32_t &raw_date) -> date_t { return raw_date; }

inline auto ImpalaTimestampToNanoseconds(const Int96 &impala_timestamp) -> int64_t {
  int64_t days_since_epoch = impala_timestamp.value[2] - JULIAN_TO_UNIX_EPOCH_DAYS;
  auto nanoseconds = Load<int64_t>(reinterpret_cast<const_data_ptr_t>(impala_timestamp.value));
  return days_since_epoch * NANOSECONDS_PER_DAY + nanoseconds;
}

/** @brief INT96 (Impala) timestamp -> timestamp_t (microseconds since epoch). */
inline auto ImpalaTimestampToTimestamp(const Int96 &raw_ts) -> timestamp_t {
  auto impala_ns = ImpalaTimestampToNanoseconds(raw_ts);
  return Timestamp::FromEpochNano(impala_ns);
}

/** @brief timestamp_t -> INT96 (Impala) representation, used by the writer. */
inline auto TimestampToImpalaTimestamp(const timestamp_t &ts) -> Int96 {
  // Micros within the day -> nanoseconds since midnight.
  auto micros_since_midnight = Timestamp::GetTime(ts);
  auto days_since_epoch = Date::Epoch(Timestamp::GetDate(ts)) / SECONDS_PER_DAY;
  Int96 impala_ts;
  Store<uint64_t>(static_cast<uint64_t>(micros_since_midnight) * 1000, reinterpret_cast<data_ptr_t>(impala_ts.value));
  impala_ts.value[2] = static_cast<uint32_t>(days_since_epoch + JULIAN_TO_UNIX_EPOCH_DAYS);
  return impala_ts;
}

inline auto ParquetTimestampMicrosToTimestamp(const int64_t &raw_ts) -> timestamp_t {
  return Timestamp::FromEpochMicroSeconds(raw_ts);
}

inline auto ParquetTimestampMsToTimestamp(const int64_t &raw_ts) -> timestamp_t {
  return Timestamp::FromEpochMs(raw_ts);
}

}  // namespace bumblebee
