//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bswap.h
//
// Identification: src/include/common/bswap.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace bumblebee {

#define BSWAP16(x) ((uint16_t)((((uint16_t)(x) & 0xff00) >> 8) | (((uint16_t)(x) & 0x00ff) << 8)))

#define BSWAP32(x)                                                                        \
  ((uint32_t)((((uint32_t)(x) & 0xff000000) >> 24) | (((uint32_t)(x) & 0x00ff0000) >> 8) | \
              (((uint32_t)(x) & 0x0000ff00) << 8) | (((uint32_t)(x) & 0x000000ff) << 24)))

#define BSWAP64(x)                                                                                            \
  ((uint64_t)((((uint64_t)(x) & 0xff00000000000000ull) >> 56) | (((uint64_t)(x) & 0x00ff000000000000ull) >> 40) | \
              (((uint64_t)(x) & 0x0000ff0000000000ull) >> 24) | (((uint64_t)(x) & 0x000000ff00000000ull) >> 8) |  \
              (((uint64_t)(x) & 0x00000000ff000000ull) << 8) | (((uint64_t)(x) & 0x0000000000ff0000ull) << 24) |  \
              (((uint64_t)(x) & 0x000000000000ff00ull) << 40) | (((uint64_t)(x) & 0x00000000000000ffull) << 56)))

/** @brief Byte-swap an 8-bit value (identity). */
inline auto BSwap(const uint8_t &x) -> uint8_t { return x; }

/** @brief Byte-swap a 16-bit value. */
inline auto BSwap(const uint16_t &x) -> uint16_t { return BSWAP16(x); }

/** @brief Byte-swap a 32-bit value. */
inline auto BSwap(const uint32_t &x) -> uint32_t { return BSWAP32(x); }

/** @brief Byte-swap a 64-bit value. */
inline auto BSwap(const uint64_t &x) -> uint64_t { return BSWAP64(x); }

/** @brief Byte-swap a signed 64-bit value. */
inline auto BSwap(const int64_t &x) -> int64_t { return static_cast<int64_t>(BSWAP64(x)); }

}  // namespace bumblebee
