//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// hash.cpp
//
// Identification: src/common/hash.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/hash.h"

#include <cstring>
#include <functional>

namespace bumblebee {

template <>
auto Hash(uint64_t val) -> hash_t {
  return MurmurHash64(val);
}

template <>
auto Hash(int64_t val) -> hash_t {
  return MurmurHash64(static_cast<uint64_t>(val));
}

template <>
auto Hash(float val) -> hash_t {
  return std::hash<float>{}(val);
}

template <>
auto Hash(double val) -> hash_t {
  return std::hash<double>{}(val);
}

template <>
auto Hash(const char *str) -> hash_t {
  return Hash(str, strlen(str));
}

template <>
auto Hash(std::string val) -> hash_t {
  return Hash(val.c_str(), val.size());
}

template <>
auto Hash(char *val) -> hash_t {
  return Hash<const char *>(val);
}

// Word-at-a-time string hash (FNV-style accumulation with a strong finalizer).
// Processes 8 bytes per iteration instead of 1, which dominates grouping/join performance
// on string keys (e.g. URL GROUP BY). The hash value is used only for bucketing and bloom
// filters — equality is always re-checked — so the exact algorithm is a pure
// performance/distribution choice and cannot change query results.
auto Hash(const char *val, size_t size) -> hash_t {
  const uint64_t m = 0x9E3779B97F4A7C15ULL;  // golden-ratio odd multiplier
  uint64_t h = 0xcbf29ce484222325ULL ^ (size * m);
  const char *p = val;
  size_t n = size;
  while (n >= 8) {
    uint64_t k;
    memcpy(&k, p, 8);
    h ^= k;
    h *= m;
    h ^= h >> 29;
    p += 8;
    n -= 8;
  }
  if (n != 0) {
    // Read the remaining 1..7 bytes into a zero-initialised word.
    uint64_t k = 0;
    memcpy(&k, p, n);
    h ^= k;
    h *= m;
  }
  // Avalanche finalizer.
  h ^= h >> 32;
  h *= m;
  h ^= h >> 29;
  return h;
}

}  // namespace bumblebee
