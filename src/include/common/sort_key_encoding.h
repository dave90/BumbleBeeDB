//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// sort_key_encoding.h
//
// Identification: src/include/common/sort_key_encoding.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cfloat>
#include <climits>
#include <cstdint>
#include <type_traits>

#include "common/bswap.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/helper.h"

namespace bumblebee {

/**
 * Turns a value into bytes that sort, under a plain memcmp, exactly as the value itself
 * sorts under `<`.
 *
 * That is the whole point of a sort key: once every column of a row is encoded and the
 * encodings concatenated, a multi-column ORDER BY collapses into one byte comparison, and
 * the sorter never has to look at the types again.
 *
 * Two things stand in the way of memcmp ordering, and each has a fix here:
 *  - byte order: the bytes are stored big-endian, so the most significant byte compares
 *    first (BSwap on a little-endian machine);
 *  - the sign bit: a two's-complement negative number has its top bit set, which would
 *    make it compare ABOVE every positive number. Flipping that bit restores the order.
 *    Floats need more than a bit flip — see EncodeFloat / EncodeDouble.
 */
struct SortKeyEncoding {
 public:
  /** @brief Write the byte-comparable encoding of `value` at `dataptr`. */
  template <class T>
  static inline void EncodeData(data_ptr_t dataptr, T value) {
    (void)dataptr;
    (void)value;
    throw NotImplementedException("SortKeyEncoding: cannot build a sort key from this type");
  }

  /** @brief Flip the sign bit, so that negatives sort below positives. */
  static inline auto FlipSign(uint8_t key_byte) -> uint8_t { return key_byte ^ 128; }

  /**
   * @brief Map a float onto a uint32 whose unsigned order matches the float's order.
   *
   * IEEE-754 already orders positive floats correctly when read as an integer; the
   * negatives are reversed, so they get a full one's complement, and the positives get
   * their sign bit set so they sit above every negative.
   */
  static inline auto EncodeFloat(float x) -> uint32_t {
    uint32_t buff;
    // Zero.
    if (x == 0) {
      buff = 0;
      buff |= (1U << 31);
      return buff;
    }
    // +infinity.
    if (x > FLT_MAX) {
      return UINT_MAX - 1;
    }
    // -infinity.
    if (x < -FLT_MAX) {
      return 0;
    }
    buff = Load<uint32_t>(reinterpret_cast<const_data_ptr_t>(&x));
    if ((buff & (1U << 31)) == 0) {
      // +0 and the positive numbers.
      buff |= (1U << 31);
    } else {
      // The negative numbers: one's complement.
      buff = ~buff;
    }
    return buff;
  }

  /** @brief Map a double onto a uint64. See EncodeFloat. */
  static inline auto EncodeDouble(double x) -> uint64_t {
    uint64_t buff;
    // Zero.
    if (x == 0) {
      buff = 0;
      buff += (1ULL << 63);
      return buff;
    }
    // +infinity.
    if (x > DBL_MAX) {
      return ULLONG_MAX - 1;
    }
    // -infinity.
    if (x < -DBL_MAX) {
      return 0;
    }
    buff = Load<uint64_t>(reinterpret_cast<const_data_ptr_t>(&x));
    if (buff < (1ULL << 63)) {
      // +0 and the positive numbers.
      buff += (1ULL << 63);
    } else {
      // The negative numbers: one's complement.
      buff = ~buff;
    }
    return buff;
  }

 private:
  template <class T>
  static void EncodeSigned(data_ptr_t dataptr, T value);
};

template <class T>
void SortKeyEncoding::EncodeSigned(data_ptr_t dataptr, T value) {
  using unsigned_type = std::make_unsigned_t<T>;
  unsigned_type bytes;
  Store<T>(value, reinterpret_cast<data_ptr_t>(&bytes));
  Store<unsigned_type>(BSwap(bytes), dataptr);
  dataptr[0] = FlipSign(dataptr[0]);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, int8_t value) {
  EncodeSigned<int8_t>(dataptr, value);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, int16_t value) {
  EncodeSigned<int16_t>(dataptr, value);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, int32_t value) {
  EncodeSigned<int32_t>(dataptr, value);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, int64_t value) {
  EncodeSigned<int64_t>(dataptr, value);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, uint8_t value) {
  Store<uint8_t>(value, dataptr);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, uint16_t value) {
  Store<uint16_t>(BSwap(value), dataptr);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, uint32_t value) {
  Store<uint32_t>(BSwap(value), dataptr);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, uint64_t value) {
  Store<uint64_t>(BSwap(value), dataptr);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, float value) {
  uint32_t converted_value = EncodeFloat(value);
  Store<uint32_t>(BSwap(converted_value), dataptr);
}

template <>
inline void SortKeyEncoding::EncodeData(data_ptr_t dataptr, double value) {
  uint64_t converted_value = EncodeDouble(value);
  Store<uint64_t>(BSwap(converted_value), dataptr);
}

}  // namespace bumblebee
