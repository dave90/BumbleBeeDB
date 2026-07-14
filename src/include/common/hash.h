//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// hash.h
//
// Identification: src/include/common/hash.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

#include "common/config.h"
#include "common/macros.h"
#include "type/bumble_string.h"

namespace bumblebee {

/**
 * @brief Mix a 64-bit value so that the output looks uniform.
 *
 * Maximizes the avalanche effect and minimizes bias.
 * See: https://nullprogram.com/blog/2018/07/31/
 */
inline auto MurmurHash64(uint64_t x) -> hash_t { return x * UINT64_C(0xbf58476d1ce4e5b9); }

/** @brief Mix a 32-bit value so that the output looks uniform. */
inline auto MurmurHash32(uint32_t x) -> hash_t { return MurmurHash64(x); }

/** @brief Hash a value. Specialized below for every type the engine stores. */
template <class T>
auto Hash(T value) -> hash_t {
  return MurmurHash32(value);
}

/** @brief Combine two hashes by XORing them. */
inline auto CombineHash(hash_t left, hash_t right) -> hash_t { return left ^ right; }

template <>
auto Hash(uint64_t val) -> hash_t;
template <>
auto Hash(int64_t val) -> hash_t;
template <>
auto Hash(float val) -> hash_t;
template <>
auto Hash(double val) -> hash_t;
template <>
auto Hash(const char *val) -> hash_t;
template <>
auto Hash(char *val) -> hash_t;
template <>
auto Hash(std::string val) -> hash_t;

/**
 * @brief Hash `size` bytes of `val`.
 *
 * @param val The bytes to hash.
 * @param size The number of bytes.
 * @return hash_t The hash.
 */
auto Hash(const char *val, size_t size) -> hash_t;

// Inline so the per-row hashing loops in group-by/join don't pay an out-of-line call
// (plus a by-value string_t copy) per value; the actual byte-mixing loop stays in
// Hash(const char *, size_t).
template <>
inline auto Hash(string_t val) -> hash_t {
  return Hash(val.GetDataUnsafe(), val.Size());
}

/** @brief A std::hash-compatible hasher over std::string using the engine's string hash. */
struct StringTHash {
  auto operator()(const std::string &v) const noexcept -> size_t { return Hash<std::string>(v); }
};

/** @brief Avalanche mixer: scrambles bits so outputs look uniform even if inputs don't. */
inline auto Mix64(uint64_t x) -> uint64_t {
  x ^= x >> 30;                // fold high bits into low
  x *= 0xbf58476d1ce4e5b9ULL;  // large odd multiplier -> nonlinear diffusion
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;  // second independent multiplier
  x ^= x >> 31;
  return x;
}

/**
 * @brief Derive two independent-looking 32-bit values from one 64-bit hash.
 *
 * Uses the golden-ratio constant to decorrelate the second stream.
 *
 * @param h The source hash.
 * @param h1 Out: the first derived value.
 * @param h2 Out: the second derived value, forced odd to avoid cycles.
 */
inline void DeriveH1H2(uint64_t h, uint32_t &h1, uint32_t &h2) {
  uint64_t a = Mix64(h);
  uint64_t b = Mix64(h ^ 0x9e3779b97f4a7c15ULL);  // golden ratio constant
  h1 = static_cast<uint32_t>(a);                  // low 32 bits (already mixed)
  h2 = static_cast<uint32_t>(b | 1U);             // low 32 bits, forced odd to avoid cycles
}

/**
 * @brief Build k positions in a 16-bit Bloom filter via double hashing.
 *
 * In practice for m=16: k=2 with very few items per filter, k=3-4 with 3-5 items, k=5+ if
 * many items are expected — but then the 16-bit filter saturates quickly.
 *
 * @param h The hash of the item.
 * @param k The number of bits to set, in [1, 16].
 * @return uint16_t The bit mask of the item.
 */
inline auto Bloom16FromHash(uint64_t h, int k) -> uint16_t {
  BUMBLEBEE_ASSERT(k > 0 && k <= 16, "bloom filter k out of range");
  uint32_t h1;
  uint32_t h2;
  DeriveH1H2(h, h1, h2);
  uint16_t mask = 0;
  for (int i = 0; i < k; ++i) {
    // modulo 16 via bitmask (m must be a power of two)
    uint32_t pos = (h1 + static_cast<uint32_t>(i) * h2) & 15U;
    mask |= static_cast<uint16_t>(1U << pos);
  }
  return mask;
}

/** @brief Set the bits of `hash` in the 16-bit Bloom filter `bloom`. */
inline void Bloom16Add(uint16_t &bloom, uint64_t hash, int k = 4) { bloom |= Bloom16FromHash(hash, k); }

/** @return True if every bit of `hash` is present in `bloom`, i.e. it may contain the item. */
inline auto Bloom16CouldContain(uint16_t bloom, uint64_t hash, int k = 4) -> bool {
  uint16_t mask = Bloom16FromHash(hash, k);
  return (mask & ~bloom) == 0;  // all required bits set?
}

}  // namespace bumblebee
