//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// helper.h
//
// Identification: src/include/common/helper.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "common/config.h"
#include "common/hash.h"

namespace bumblebee {

/** @return The larger of `a` and `b`. */
template <typename T>
auto MaxValue(T a, T b) -> T {
  return a > b ? a : b;
}

/** @return The smaller of `a` and `b`. */
template <typename T>
auto MinValue(T a, T b) -> T {
  return a < b ? a : b;
}

/** @return The absolute value of `a`. */
template <typename T>
auto AbsValue(T a) -> T {
  return a < 0 ? -a : a;
}

/** @return `n` rounded up to the next multiple of `val`. */
template <class T, T val = 8>
auto AlignValue(T n) -> T {
  return ((n + (val - 1)) / val) * val;
}

/** @return True if `n` is a multiple of `val`. */
template <class T, T val = 8>
auto ValueIsAligned(T n) -> bool {
  return (n % val) == 0;
}

/** @return -1 if `a` is negative, 1 otherwise. */
template <typename T>
auto SignValue(T a) -> T {
  return a < 0 ? -1 : 1;
}

/** @brief Copy the value of the first element of `ptr` out of the raw bytes. */
template <typename T>
auto Load(const_data_ptr_t ptr) -> T {
  T ret;
  memcpy(&ret, ptr, sizeof(ret));
  return ret;
}

/** @brief Store `val` into the raw bytes at `ptr`. */
template <typename T>
void Store(const T val, data_ptr_t ptr) {
  memcpy(ptr, static_cast<const void *>(&val), sizeof(val));
}

/**
 * @brief Assign a shared pointer, but ONLY if `target` is not already `source`.
 *
 * When that is often the case this is significantly faster (~20x), because it avoids an
 * atomic incref/decref at the cost of a single pointer comparison.
 */
template <class T>
void AssignSharedPointer(std::shared_ptr<T> &target, const std::shared_ptr<T> &source) {
  if (target.get() != source.get()) {
    target = source;
  }
}

/** @return True if the two vectors hold the same elements, in any order. */
template <typename T>
auto CompareVectors(std::vector<T> v1, std::vector<T> v2) -> bool {
  if (v1.size() != v2.size()) {
    return false;
  }
  std::sort(v1.begin(), v1.end());
  std::sort(v2.begin(), v2.end());
  return v1 == v2;
}

/** @return True if `v` contains `val`. */
template <typename T>
auto ContainsVector(const std::vector<T> &v, const T &val) -> bool {
  return std::find(v.begin(), v.end(), val) != v.end();
}

/**
 * @brief Compare two vectors as multisets, without requiring the element type to be sortable.
 *
 * @return True if the two vectors hold the same elements, in any order.
 */
template <typename T>
auto CompareVectorsNoSort(const std::vector<T> &lhs, const std::vector<T> &rhs) -> bool {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  std::vector<bool> used(lhs.size(), false);
  bool matched = false;
  for (const auto &x : lhs) {
    matched = false;
    for (std::size_t j = 0; j < rhs.size(); ++j) {
      if (!used[j] && x == rhs[j]) {
        used[j] = true;  // consume this occurrence
        matched = true;
        break;
      }
    }
    if (!matched) {
      break;  // x has no unused match in rhs
    }
  }
  return matched;
}

/**
 * @brief Compute a checksum over a buffer.
 *
 * @param buffer The bytes to check-sum.
 * @param size The number of bytes.
 * @return uint64_t The checksum.
 */
inline auto CalcChecksum(uint8_t *buffer, size_t size) -> uint64_t {
  uint64_t result = 5381;
  auto *ptr = reinterpret_cast<uint64_t *>(buffer);
  size_t i;
  // For efficiency, first hash whole uint64_t values.
  for (i = 0; i < size / 8; i++) {
    result ^= Hash(ptr[i]);
  }
  if (size - i * 8 > 0) {
    // The remaining 0-7 bytes are hashed with the string hash.
    result ^= Hash(reinterpret_cast<const char *>(buffer + i * 8), size - i * 8);
  }
  return result;
}

}  // namespace bumblebee
