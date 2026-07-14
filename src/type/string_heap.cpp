//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_heap.cpp
//
// Identification: src/type/string_heap.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/string_heap.h"

#include "common/macros.h"

namespace bumblebee {

auto StringHeap::AddBlob(const char *data, idx_t len) -> string_t {
  BUMBLEBEE_ASSERT(len <= MINIMUM_HEAP_SIZE, "String too large for chunk");
  auto new_string = AddEmptyString(len);
  memcpy(new_string.GetDataWriteable(), data, len);
  new_string.GetDataWriteable()[len] = '\0';
  return new_string;
}

auto StringHeap::AddEmptyString(idx_t len) -> string_t {
  BUMBLEBEE_ASSERT(len <= MINIMUM_HEAP_SIZE, "String too large for chunk");
  if (!chunk_ || chunk_->current_position_ + len + 1 >= chunk_->maximum_size_) {
    // Create a new chunk. NOTE: the maximum string length supported is MINIMUM_HEAP_SIZE.
    auto new_chunk = std::make_unique<StringChunk>(MINIMUM_HEAP_SIZE);
    if (chunk_) {
      new_chunk->prev_ = std::move(chunk_);
    }
    chunk_ = std::move(new_chunk);
  }
  char *data_ptr = chunk_->data_.get() + chunk_->current_position_;
  chunk_->current_position_ += len + 1;  // +1 for the NUL termination
  // Add the NUL termination up front so that the reserved bytes read as an empty string.
  data_ptr[0] = '\0';
  return {data_ptr, static_cast<uint32_t>(len)};
}

}  // namespace bumblebee
