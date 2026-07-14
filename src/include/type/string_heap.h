//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_heap.h
//
// Identification: src/include/type/string_heap.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstring>
#include <memory>
#include <string>

#include "common/config.h"
#include "type/bumble_string.h"

namespace bumblebee {

/**
 * The owner of the bytes a set of BumbleStrings points at.
 *
 * Strings are bump-allocated into fixed-size chunks of MINIMUM_HEAP_SIZE bytes. A chunk is
 * never freed while the heap lives, and a string is never split across chunks, so the
 * largest string the heap can store is MINIMUM_HEAP_SIZE bytes.
 */
class StringHeap {
 public:
  StringHeap() = default;
  StringHeap(StringHeap &&other) noexcept : chunk_(std::move(other.chunk_)) {}
  auto operator=(StringHeap &&other) noexcept -> StringHeap & {
    chunk_ = std::move(other.chunk_);
    return *this;
  }
  StringHeap(const StringHeap &) = delete;
  auto operator=(const StringHeap &) -> StringHeap & = delete;
  ~StringHeap() = default;

  /** @brief Release every chunk. All strings previously handed out are invalidated. */
  void Destroy() { chunk_ = nullptr; }

  /** @brief Copy `len` bytes of `data` into the heap. */
  auto AddString(const char *data, idx_t len) -> string_t { return AddBlob(data, len); }

  /** @brief Copy the NUL-terminated string `data` into the heap. */
  auto AddString(const char *data) -> string_t { return AddString(data, strlen(data)); }

  /** @brief Copy `data` into the heap. */
  auto AddString(const std::string &data) -> string_t { return AddString(data.c_str(), data.length()); }

  /** @brief Copy the bytes of `data` into the heap. */
  auto AddString(const string_t &data) -> string_t { return AddString(data.CStr(), data.Length()); }

  /**
   * @brief Copy `len` raw bytes into the heap.
   *
   * Note: the NUL termination of the source is not copied; a terminator is written after
   * the `len` bytes.
   *
   * @param data The bytes to copy.
   * @param len The number of bytes. At most MINIMUM_HEAP_SIZE.
   * @return string_t A string over the heap-owned copy.
   */
  auto AddBlob(const char *data, idx_t len) -> string_t;

  /**
   * @brief Reserve room for a string of `len` bytes, whose content is written by the caller.
   *
   * @param len The number of bytes. At most MINIMUM_HEAP_SIZE.
   * @return string_t A string over the reserved (uninitialized) bytes.
   */
  auto AddEmptyString(idx_t len) -> string_t;

 private:
  /** One bump-allocated block of string bytes, linked to the block that preceded it. */
  struct StringChunk {
    explicit StringChunk(idx_t size) : maximum_size_(size) { data_ = std::unique_ptr<char[]>(new char[maximum_size_]); }

    std::unique_ptr<char[]> data_;
    idx_t current_position_{0};
    idx_t maximum_size_{0};
    /** The chunk that was full before this one. */
    std::unique_ptr<StringChunk> prev_;
  };

  std::unique_ptr<StringChunk> chunk_;
};

}  // namespace bumblebee
