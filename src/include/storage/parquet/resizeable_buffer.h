//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// resizeable_buffer.h
//
// Identification: src/include/storage/parquet/resizeable_buffer.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstring>

#include "common/allocator.h"
#include "common/exception.h"
#include "common/helper.h"

namespace bumblebee {

/**
 * @brief A bounds-checked cursor over a byte range. `ptr_`/`len_` advance as bytes are consumed;
 * every access verifies the remaining length so a corrupt page fails loudly.
 */
class ByteBuffer {
 public:
  ByteBuffer() = default;
  ByteBuffer(char *ptr, uint64_t len) : ptr_(ptr), len_(len) {}
  virtual ~ByteBuffer() = default;

  char *ptr_ = nullptr;
  uint64_t len_ = 0;

  /** @brief Consume `increment` bytes. */
  void Inc(uint64_t increment) {
    Available(increment);
    len_ -= increment;
    ptr_ += increment;
  }

  /** @brief Read one fixed-width value and consume it. */
  template <class T>
  auto Read() -> T {
    T val = Get<T>();
    Inc(sizeof(T));
    return val;
  }

  /** @brief Read one fixed-width value without consuming it. */
  template <class T>
  auto Get() -> T {
    Available(sizeof(T));
    return Load<T>(reinterpret_cast<data_ptr_t>(ptr_));
  }

  /** @brief Copy `len` bytes out without consuming them. */
  void CopyTo(char *dest, uint64_t len) {
    Available(len);
    std::memcpy(dest, ptr_, len);
  }

  /** @brief Zero the remaining bytes. */
  void Zero() { std::memset(ptr_, 0, len_); }

  /** @brief Throw unless at least `req_len` bytes remain. */
  void Available(uint64_t req_len) {
    if (req_len > len_) {
      throw Exception("Parquet reader: out of buffer (corrupt page?)");
    }
  }
};

/** @brief A ByteBuffer over memory owned through an Allocator, resizable between uses. */
class ResizeableBuffer : public ByteBuffer {
 public:
  ResizeableBuffer() = default;
  ResizeableBuffer(Allocator &allocator, uint64_t new_size) { Resize(allocator, new_size); }

  /** @brief Ensure capacity for `new_size` bytes and reset the cursor to the allocation base. */
  void Resize(Allocator &allocator, uint64_t new_size) {
    len_ = new_size;
    if (new_size == 0) {
      return;
    }
    if (new_size > alloc_len_) {
      alloc_len_ = new_size;
      allocated_data_ = allocator.Allocate(alloc_len_);
    }
    // Always reset ptr_ to the allocation base. A previous user of this buffer may have
    // advanced ptr_ via Inc()/Read() while consuming a page; when the buffer is reused
    // without reallocating, ptr_ must point back at the start or the next writer (e.g. page
    // decompression) would run past the end of the allocation.
    ptr_ = reinterpret_cast<char *>(allocated_data_->Get());
  }

 private:
  alloc_data_ptr_t allocated_data_;
  idx_t alloc_len_ = 0;
};

}  // namespace bumblebee
