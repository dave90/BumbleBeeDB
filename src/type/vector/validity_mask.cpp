//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// validity_mask.cpp
//
// Identification: src/type/vector/validity_mask.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/validity_mask.h"

#include <cstring>

#include "common/helper.h"
#include "type/vector/selection_vector.h"

namespace bumblebee {

void ValidityMask::EnsureWritable(idx_t capacity) {
  // Always cover at least a full standard vector so that bulk readers (slice/copy) never
  // run past the buffer for a normal-sized vector.
  if (capacity < STANDARD_VECTOR_SIZE) {
    capacity = STANDARD_VECTOR_SIZE;
  }
  idx_t words = WordCount(capacity);
  if (mask_ != nullptr && capacity_words_ >= words) {
    // Already allocated and large enough.
    return;
  }
  // (Re)allocate an all-ones (all-valid) buffer, preserving the existing bits.
  auto new_data = std::shared_ptr<uint64_t[]>(new uint64_t[words]);
  std::memset(new_data.get(), 0xFF, words * sizeof(uint64_t));
  if (mask_ != nullptr) {
    std::memcpy(new_data.get(), mask_, capacity_words_ * sizeof(uint64_t));
  }
  owned_data_ = std::move(new_data);
  mask_ = owned_data_.get();
  capacity_words_ = words;
}

void ValidityMask::SetAllValid() {
  owned_data_.reset();
  mask_ = nullptr;
  capacity_words_ = 0;
}

void ValidityMask::SetAllInvalid(idx_t count) {
  EnsureWritable(count);
  std::memset(mask_, 0x00, capacity_words_ * sizeof(uint64_t));
}

auto ValidityMask::CheckAllValid(idx_t count) const -> bool {
  if (mask_ == nullptr) {
    return true;
  }
  for (idx_t i = 0; i < count; i++) {
    if (!RowIsValid(i)) {
      return false;
    }
  }
  return true;
}

auto ValidityMask::Copy() const -> ValidityMask {
  ValidityMask result;
  if (mask_ != nullptr) {
    result.EnsureWritable(capacity_words_ * BITS_PER_WORD);
    std::memcpy(result.mask_, mask_, capacity_words_ * sizeof(uint64_t));
  }
  return result;
}

void ValidityMask::Slice(const ValidityMask &other, idx_t offset, idx_t count) {
  // Produce an independent buffer (do not alias a shared mask).
  SetAllValid();
  if (other.AllValid()) {
    return;
  }
  // Bit offsets are generally not word-aligned: materialize an owned copy. Bound the read
  // by the source's covered range; rows past it are implicitly valid.
  EnsureWritable(count);
  idx_t src_bits = other.capacity_words_ * BITS_PER_WORD;
  idx_t limit = (offset < src_bits) ? MinValue(count, src_bits - offset) : 0;
  for (idx_t i = 0; i < limit; i++) {
    Set(i, other.RowIsValid(offset + i));
  }
}

void ValidityMask::GatherFrom(const ValidityMask &other, const SelectionVector &sel, idx_t count) {
  // Produce an independent buffer (do not alias a shared mask).
  SetAllValid();
  if (other.AllValid()) {
    // Nothing to gather: leave this all-valid.
    return;
  }
  EnsureWritable(count);
  for (idx_t i = 0; i < count; i++) {
    Set(i, other.RowIsValid(sel.GetIndex(i)));
  }
}

void ValidityMask::Combine(const ValidityMask &other, idx_t count) {
  if (other.AllValid()) {
    return;
  }
  EnsureWritable(count);
  for (idx_t i = 0; i < count; i++) {
    if (!other.RowIsValid(i)) {
      SetInvalid(i);
    }
  }
}

}  // namespace bumblebee
