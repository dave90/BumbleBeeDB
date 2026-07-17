//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// byte_buffer.h
//
// Identification: src/include/common/byte_buffer.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/exception.h"

namespace bumblebee {

/**
 * @brief A minimal append-only byte encoder for persisting metadata (the catalog record, schemas).
 *
 * Fixed-width values are stored in native byte order via `memcpy` — sufficient for a single-host DB
 * file; cross-endian portability is out of scope. Strings are length-prefixed (`u32` length + bytes).
 */
class ByteWriter {
 public:
  void PutU8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void PutU16(uint16_t v) { PutRaw(&v, sizeof(v)); }
  void PutU32(uint32_t v) { PutRaw(&v, sizeof(v)); }
  void PutI32(int32_t v) { PutRaw(&v, sizeof(v)); }
  void PutU64(uint64_t v) { PutRaw(&v, sizeof(v)); }
  void PutI64(int64_t v) { PutRaw(&v, sizeof(v)); }
  void PutString(const std::string &s) {
    PutU32(static_cast<uint32_t>(s.size()));
    PutRaw(s.data(), s.size());
  }

  auto Data() const -> const std::vector<char> & { return buf_; }
  auto Size() const -> size_t { return buf_.size(); }

 private:
  void PutRaw(const void *p, size_t n) {
    auto *bytes = static_cast<const char *>(p);
    buf_.insert(buf_.end(), bytes, bytes + n);
  }
  std::vector<char> buf_;
};

/**
 * @brief The cursor-based reader matching `ByteWriter`. Every read bounds-checks and throws on
 * underflow, so a truncated / corrupt record fails loudly rather than reading past the buffer.
 */
class ByteReader {
 public:
  ByteReader(const char *data, size_t size) : data_(data), size_(size) {}

  auto GetU8() -> uint8_t {
    Need(1);
    return static_cast<uint8_t>(data_[pos_++]);
  }
  auto GetU16() -> uint16_t { return GetRaw<uint16_t>(); }
  auto GetU32() -> uint32_t { return GetRaw<uint32_t>(); }
  auto GetI32() -> int32_t { return GetRaw<int32_t>(); }
  auto GetU64() -> uint64_t { return GetRaw<uint64_t>(); }
  auto GetI64() -> int64_t { return GetRaw<int64_t>(); }
  auto GetString() -> std::string {
    auto len = GetU32();
    Need(len);
    std::string s(data_ + pos_, len);
    pos_ += len;
    return s;
  }

  auto Remaining() const -> size_t { return size_ - pos_; }
  auto Pos() const -> size_t { return pos_; }

 private:
  void Need(size_t n) const {
    if (pos_ + n > size_) {
      throw Exception("ByteReader: read past end of buffer (truncated or corrupt record)");
    }
  }
  template <class T>
  auto GetRaw() -> T {
    Need(sizeof(T));
    T v;
    std::memcpy(&v, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  const char *data_;
  size_t size_;
  size_t pos_{0};
};

}  // namespace bumblebee
