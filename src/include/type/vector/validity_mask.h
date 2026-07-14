//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// validity_mask.h
//
// Identification: src/include/type/vector/validity_mask.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>

#include "common/config.h"
#include "common/macros.h"

namespace bumblebee {

class SelectionVector;

/**
 * Per-row validity bitmask. A set bit (1) means the row is VALID, i.e. NOT null.
 *
 * A null/empty buffer means "all valid" — zero memory, zero overhead, which is the common
 * case. The buffer is allocated lazily on the first write of a null via EnsureWritable().
 *
 * The backing buffer is held by a shared_ptr so that copying a mask (e.g. when a Vector
 * references another) shares the buffer cheaply, matching the "referenced vectors share
 * their data" model. Slice() / GatherFrom() always produce an independent buffer.
 *
 * NOTE: critical class — the hot-path accessors are inlined here.
 */
class ValidityMask {
 public:
  static constexpr idx_t BITS_PER_WORD = 64;

  ValidityMask() = default;
  ValidityMask(const ValidityMask &other) = default;                     // shallow: shares the buffer
  auto operator=(const ValidityMask &other) -> ValidityMask & = default; // shallow: shares the buffer
  ValidityMask(ValidityMask &&) noexcept = default;
  auto operator=(ValidityMask &&) noexcept -> ValidityMask & = default;
  ~ValidityMask() = default;

  /** @return ValidityMask A deep copy: an independent buffer holding the same bits. */
  auto Copy() const -> ValidityMask;

  /** @return True when the buffer is absent, i.e. every row is valid. */
  auto AllValid() const -> bool { return mask_ == nullptr; }

  /** @return The raw bit words, or nullptr when all-valid. */
  auto Data() const -> const uint64_t * { return mask_; }

  /** @return The raw bit words, or nullptr when all-valid. */
  auto Data() -> uint64_t * { return mask_; }

  /**
   * @brief True if the row is valid (not null).
   *
   * All-valid fast path: no buffer means everything is valid. An idx beyond the allocated
   * buffer is treated as valid (the buffer was only ever grown by SetInvalid up to whatever
   * index the caller passed; rows past that high-water mark have not been observed as null
   * and therefore default to valid).
   */
  auto RowIsValid(idx_t idx) const -> bool {
    if (mask_ == nullptr) {
      return true;
    }
    if (idx / BITS_PER_WORD >= capacity_words_) {
      return true;
    }
    return (mask_[idx / BITS_PER_WORD] & (uint64_t(1) << (idx % BITS_PER_WORD))) != 0;
  }

  /**
   * @brief Mark a row valid.
   *
   * No-op when all-valid (nothing is null yet) or when the row is past the allocated
   * buffer (which would already report valid via RowIsValid).
   */
  void SetValid(idx_t idx) {
    if (mask_ == nullptr) {
      return;
    }
    if (idx / BITS_PER_WORD >= capacity_words_) {
      return;
    }
    mask_[idx / BITS_PER_WORD] |= (uint64_t(1) << (idx % BITS_PER_WORD));
  }

  /** @brief Mark a row invalid (null). Lazily allocates the buffer on first use. */
  void SetInvalid(idx_t idx) {
    EnsureWritable(idx + 1);
    // Invariant: EnsureWritable must have grown the buffer to cover idx.
    BUMBLEBEE_ASSERT(mask_ != nullptr && idx / BITS_PER_WORD < capacity_words_, "validity buffer too small");
    mask_[idx / BITS_PER_WORD] &= ~(uint64_t(1) << (idx % BITS_PER_WORD));
  }

  /** @return DEBUG invariant: a present buffer is owned and large enough for `count` rows. */
  auto DebugConsistent(idx_t count) const -> bool {
    if (mask_ == nullptr) {
      return true;
    }
    return mask_ == owned_data_.get() && capacity_words_ >= WordCount(count);
  }

  /** @brief Set the validity of a row. */
  void Set(idx_t idx, bool valid) {
    if (valid) {
      SetValid(idx);
    } else {
      SetInvalid(idx);
    }
  }

  /**
   * @brief Unchecked write accessor.
   *
   * Precondition: the writer called EnsureWritable(count) once for its full row count
   * before the loop. No unsafe read accessor exists: an input mask may be sized below its
   * vector (grown only to its last null), so reads keep the bounds-checked RowIsValid.
   */
  void SetValidUnsafe(idx_t idx) {
    BUMBLEBEE_ASSERT(mask_ != nullptr && idx / BITS_PER_WORD < capacity_words_, "validity buffer too small");
    mask_[idx / BITS_PER_WORD] |= (uint64_t(1) << (idx % BITS_PER_WORD));
  }

  /** @brief Unchecked write accessor. See SetValidUnsafe. */
  void SetInvalidUnsafe(idx_t idx) {
    BUMBLEBEE_ASSERT(mask_ != nullptr && idx / BITS_PER_WORD < capacity_words_, "validity buffer too small");
    mask_[idx / BITS_PER_WORD] &= ~(uint64_t(1) << (idx % BITS_PER_WORD));
  }

  /** @brief Drop the buffer: the mask becomes all-valid again. */
  void SetAllValid();

  /** @brief Allocate (if needed) and clear all bits in [0, count): every row becomes null. */
  void SetAllInvalid(idx_t count);

  /** @brief Drop the buffer: the mask becomes all-valid again. */
  void Reset() { SetAllValid(); }

  /** @brief Lazily allocate an all-ones (all-valid) buffer large enough for `capacity` rows. */
  void EnsureWritable(idx_t capacity = STANDARD_VECTOR_SIZE);

  /** @return True if there is no null in [0, count). */
  auto CheckAllValid(idx_t count) const -> bool;

  /**
   * @brief Make this an owned copy of bits [offset, offset + count) of `other`.
   *
   * @param other The source mask.
   * @param offset The first source row to copy.
   * @param count The number of rows to copy.
   */
  void Slice(const ValidityMask &other, idx_t offset, idx_t count = STANDARD_VECTOR_SIZE);

  /**
   * @brief Gather through a selection: this[i] = other.RowIsValid(sel[i]) for i in [0, count).
   *
   * @param other The source mask.
   * @param sel The selection to read through.
   * @param count The number of rows to gather.
   */
  void GatherFrom(const ValidityMask &other, const SelectionVector &sel, idx_t count);

  /**
   * @brief NULL propagation (logical AND): a row stays valid only if both masks say so.
   *
   * @param other The mask to AND into this one.
   * @param count The number of rows to combine.
   */
  void Combine(const ValidityMask &other, idx_t count);

 private:
  static auto WordCount(idx_t capacity) -> idx_t { return (capacity + BITS_PER_WORD - 1) / BITS_PER_WORD; }

  /** All-ones-initialized buffer (shared on copy). mask_ points into it, or is null => all valid. */
  std::shared_ptr<uint64_t[]> owned_data_;
  uint64_t *mask_{nullptr};
  idx_t capacity_words_{0};
};

}  // namespace bumblebee
