//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumble_string.h
//
// Identification: src/include/type/bumble_string.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstring>
#include <string>

#include "common/config.h"

namespace bumblebee {

/**
 * BumbleString is the physical representation of a STRING value inside a Vector.
 *
 * It is a non-owning view over string bytes that live somewhere else (a StringHeap, a
 * scan buffer, a row-layout block), with a small-string optimization: a string of at most
 * PREFIX_LENGTH bytes is stored inline in the object itself, so short strings never touch
 * the heap. Longer strings keep the first PREFIX_LENGTH bytes inline as a prefix (which
 * short-circuits most comparisons) plus a pointer to the full bytes.
 *
 * The object never allocates and never frees: the caller must keep the referenced bytes
 * alive for as long as the BumbleString is used.
 */
class BumbleString {
 public:
  /** Inline capacity. 11 so that the whole object fits in 24 bytes (a multiple of 8). */
  static constexpr idx_t PREFIX_LENGTH = 11;

  // These member functions are deliberately defined inline in the header:
  // construction, hashing and comparison of strings run once per row in scans, group-bys
  // and joins, and an out-of-line call per value dominated string-heavy query profiles.

  BumbleString() = default;

  /** @brief Construct a string of `len` bytes whose content is filled in later. */
  explicit BumbleString(uint32_t len) { value_.length = len; }

  /**
   * @brief Construct a string over `len` bytes of `data`.
   *
   * If the string is inlinable the bytes are copied into the object, otherwise `data` is
   * referenced and must outlive this object.
   */
  BumbleString(const char *data, uint32_t len) {
    value_.length = len;
    if (IsInlined()) {
      // Store the data in the prefix; +1 for the string termination.
      memcpy(value_.prefix, data, len * sizeof(char));
      value_.prefix[len] = '\0';
      return;
    }
    memcpy(value_.prefix, data, PREFIX_LENGTH * sizeof(char));
    value_.prefix[PREFIX_LENGTH] = '\0';
    value_.ptr = const_cast<char *>(data);
  }

  /** @brief Construct a string over a NUL-terminated C string. */
  BumbleString(const char *data) : BumbleString(data, strlen(data)) {}  // NOLINT(google-explicit-constructor)

  // Member-wise copy: the prefix always mirrors the referenced data, so the default copy
  // is equivalent to re-deriving it (and matches the implicitly defaulted copy assignment).
  // Keeping the type trivially copyable lets it be passed in registers.
  BumbleString(const BumbleString &other) = default;
  auto operator=(const BumbleString &other) -> BumbleString & = default;

  /** @return True if the bytes are stored inline in this object. */
  auto IsInlined() const -> bool { return IsInlined(Size()); }

  /** @return A pointer to the bytes. Not NUL-terminated in general; use Size(). */
  auto GetDataUnsafe() const -> const char * {
    if (IsInlined()) {
      return value_.prefix;
    }
    return value_.ptr;
  }

  /** @return A writable pointer to the bytes. The caller must own the referenced memory. */
  auto GetDataWriteable() const -> char * {
    if (IsInlined()) {
      return const_cast<char *>(value_.prefix);
    }
    return value_.ptr;
  }

  /** @return The inline prefix: the first min(Size(), PREFIX_LENGTH) bytes, NUL-terminated. */
  auto GetPrefix() const -> const char * { return value_.prefix; }

  /** @return The length of the string in bytes. */
  auto Length() const -> idx_t { return Size(); }

  /** @return The length of the string in bytes. */
  auto Size() const -> idx_t { return value_.length; }

  /** @return A std::string holding a copy of the bytes. */
  auto GetString() const -> std::string;

  /** @return A pointer to the bytes, for use with the C string API. */
  auto CStr() const -> const char * { return GetDataUnsafe(); }

  auto operator<(const BumbleString &r) const -> bool {
    // Compare the data: length-aware memcmp. strcmp is unusable here because
    // byte-comparable sort keys contain embedded NUL bytes.
    auto left_length = Size();
    auto right_length = r.Size();
    auto min_length = left_length < right_length ? left_length : right_length;
    auto memcmp_res = memcmp(GetDataUnsafe(), r.GetDataUnsafe(), min_length);
    return memcmp_res < 0 || (memcmp_res == 0 && left_length < right_length);
  }

  auto operator>(const BumbleString &r) const -> bool {
    auto left_length = Size();
    auto right_length = r.Size();
    auto min_length = left_length < right_length ? left_length : right_length;
    auto memcmp_res = memcmp(GetDataUnsafe(), r.GetDataUnsafe(), min_length);
    return memcmp_res > 0 || (memcmp_res == 0 && left_length > right_length);
  }

  auto operator==(const BumbleString &r) const -> bool {
    // The zero-length guard matters: it skips the (surprisingly costly) memcmp CALL for the very
    // common ''-vs-'' case — e.g. a `col <> ''` filter compares every empty row as equal-length.
    auto left_length = Size();
    return left_length == r.Size() &&
           (left_length == 0 || memcmp(GetDataUnsafe(), r.GetDataUnsafe(), left_length) == 0);
  }

  /** @return True if a string of `len` bytes is stored inline. */
  static auto IsInlined(uint32_t len) -> bool { return len <= PREFIX_LENGTH; }

 private:
  struct {
    /** +1 for the NUL termination. */
    char prefix[PREFIX_LENGTH + 1];
    /** Kept inside the struct so that the object has no padding. */
    uint32_t length;
    char *ptr;
  } value_{};
};

/** The physical type a STRING value is stored as inside a Vector. */
using string_t = BumbleString;

}  // namespace bumblebee
