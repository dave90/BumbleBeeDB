//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_page.cpp
//
// Identification: src/storage/page/table_page.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/page/table_page.h"

#include <cstring>
#include <optional>
#include <tuple>

#include "common/exception.h"

namespace bumblebee {

void TablePage::Init() {
  next_page_id_ = INVALID_PAGE_ID;
  num_tuples_ = 0;
  num_deleted_tuples_ = 0;
}

auto TablePage::GetNextRowOffset(uint16_t row_size) const -> std::optional<uint16_t> {
  size_t slot_end_offset;
  if (num_tuples_ > 0) {
    const auto &[offset, size, meta] = tuple_info_[num_tuples_ - 1];
    slot_end_offset = offset;
  } else {
    slot_end_offset = PAGE_SIZE;
  }

  auto offset_size = TABLE_PAGE_HEADER_SIZE + TUPLE_INFO_SIZE * (num_tuples_ + 1);
  // Bug #1: reject before the unsigned subtraction can wrap. A row larger than the space left
  // (or than the page) never yields a bogus offset that would corrupt the heap on memcpy.
  if (static_cast<size_t>(row_size) > slot_end_offset || slot_end_offset - row_size < offset_size) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(slot_end_offset - row_size);
}

auto TablePage::InsertRow(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size) -> std::optional<uint16_t> {
  auto row_offset = GetNextRowOffset(row_size);
  if (!row_offset.has_value()) {
    return std::nullopt;
  }
  auto slot = num_tuples_;
  tuple_info_[slot] = std::make_tuple(*row_offset, row_size, meta);
  num_tuples_++;
  std::memcpy(page_start_ + *row_offset, row_data, row_size);
  return slot;
}

void TablePage::UpdateTupleMeta(const TupleMeta &meta, uint16_t slot) {
  if (slot >= num_tuples_) {
    throw Exception("Slot out of range");
  }
  auto &[offset, size, old_meta] = tuple_info_[slot];
  if (!old_meta.is_deleted_ && meta.is_deleted_) {
    num_deleted_tuples_++;
  }
  tuple_info_[slot] = std::make_tuple(offset, size, meta);
}

auto TablePage::GetRow(uint16_t slot) const -> std::tuple<TupleMeta, const_data_ptr_t, uint16_t> {
  if (slot >= num_tuples_) {
    throw Exception("Slot out of range");
  }
  const auto &[offset, size, meta] = tuple_info_[slot];
  return {meta, reinterpret_cast<const_data_ptr_t>(page_start_ + offset), size};
}

auto TablePage::GetTupleMeta(uint16_t slot) const -> TupleMeta {
  if (slot >= num_tuples_) {
    throw Exception("Slot out of range");
  }
  const auto &[offset, size, meta] = tuple_info_[slot];
  return meta;
}

void TablePage::UpdateRowInPlaceUnsafe(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size, uint16_t slot) {
  if (slot >= num_tuples_) {
    throw Exception("Slot out of range");
  }
  auto &[offset, size, old_meta] = tuple_info_[slot];
  if (size != row_size) {
    throw Exception("Row size mismatch");
  }
  if (!old_meta.is_deleted_ && meta.is_deleted_) {
    num_deleted_tuples_++;
  }
  tuple_info_[slot] = std::make_tuple(offset, size, meta);
  std::memcpy(page_start_ + offset, row_data, row_size);
}

auto TablePage::UpdateRow(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size, uint16_t slot) -> bool {
  if (slot >= num_tuples_) {
    throw Exception("Slot out of range");
  }
  auto [off_s, old_size, old_meta] = tuple_info_[slot];

  // Same size: nothing to shift, overwrite in place.
  if (row_size == old_size) {
    if (!old_meta.is_deleted_ && meta.is_deleted_) {
      num_deleted_tuples_++;
    }
    tuple_info_[slot] = std::make_tuple(off_s, row_size, meta);
    std::memcpy(page_start_ + off_s, row_data, row_size);
    return true;
  }

  // Rows are packed from the page's end downward in slot order, so slots below `slot` occupy the
  // contiguous block [off_last, off_s). Resizing `slot` shifts that whole block by `delta`; the
  // slot's own row and every lower slot's offset move by the same amount, keeping slots (RIDs) stable.
  const int32_t delta = static_cast<int32_t>(old_size) - static_cast<int32_t>(row_size);  // >0 shrink, <0 grow
  const uint16_t off_last = std::get<0>(tuple_info_[num_tuples_ - 1]);  // smallest offset in use
  const uint16_t below_size = static_cast<uint16_t>(off_s - off_last);  // bytes of all lower slots
  const size_t new_below_start = static_cast<size_t>(off_last) + delta;
  const size_t header_end = TABLE_PAGE_HEADER_SIZE + TUPLE_INFO_SIZE * num_tuples_;
  if (new_below_start < header_end) {
    return false;  // a grow that would collide with the slot array — does not fit the page
  }

  // Slide the block of lower rows, then write the resized row into the space that opens up. memmove
  // first is safe for both directions (a grow reads the block before the new row overwrites its top).
  if (below_size > 0) {
    std::memmove(page_start_ + new_below_start, page_start_ + off_last, below_size);
  }
  const uint16_t new_off_s = static_cast<uint16_t>(static_cast<int32_t>(off_s) + delta);
  std::memcpy(page_start_ + new_off_s, row_data, row_size);

  if (!old_meta.is_deleted_ && meta.is_deleted_) {
    num_deleted_tuples_++;
  }
  tuple_info_[slot] = std::make_tuple(new_off_s, row_size, meta);
  for (uint16_t i = slot + 1; i < num_tuples_; i++) {
    auto &[off, sz, m] = tuple_info_[i];
    off = static_cast<uint16_t>(static_cast<int32_t>(off) + delta);
  }
  return true;
}

}  // namespace bumblebee
