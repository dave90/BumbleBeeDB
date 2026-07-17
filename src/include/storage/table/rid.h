//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// rid.h
//
// Identification: src/include/storage/table/rid.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <functional>

#include "common/config.h"

namespace bumblebee {

/**
 * @brief A record identifier: the physical address of a row as `(page_id, slot)`.
 *
 * This is row-storage specific. A columnar backend would hand out a surrogate identifier
 * (e.g. `{row_group, row_offset}`) packed into the same 64 bits.
 */
class RID {
 public:
  RID() = default;

  RID(page_id_t page_id, uint32_t slot_num) : page_id_(page_id), slot_num_(slot_num) {}

  /** @brief Construct from a packed 64-bit identifier. */
  explicit RID(int64_t rid) : page_id_(static_cast<page_id_t>(rid >> 32)), slot_num_(static_cast<uint32_t>(rid)) {}

  /** @return The `(page_id, slot)` pair packed into a single 64-bit integer. */
  auto Get() const -> int64_t {
    return (static_cast<int64_t>(page_id_) << 32) | static_cast<int64_t>(slot_num_);
  }

  auto GetPageId() const -> page_id_t { return page_id_; }
  auto GetSlotNum() const -> uint32_t { return slot_num_; }

  void Set(page_id_t page_id, uint32_t slot_num) {
    page_id_ = page_id;
    slot_num_ = slot_num;
  }

  auto operator==(const RID &other) const -> bool {
    return page_id_ == other.page_id_ && slot_num_ == other.slot_num_;
  }

 private:
  page_id_t page_id_{INVALID_PAGE_ID};
  uint32_t slot_num_{0};
};

}  // namespace bumblebee

namespace std {
template <>
struct hash<bumblebee::RID> {
  auto operator()(const bumblebee::RID &rid) const -> size_t { return hash<int64_t>()(rid.Get()); }
};
}  // namespace std
