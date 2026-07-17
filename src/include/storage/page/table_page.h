//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_page.h
//
// Identification: src/include/storage/page/table_page.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <optional>
#include <tuple>

#include "common/config.h"
#include "storage/table/tuple_meta.h"

namespace bumblebee {

static constexpr uint64_t TABLE_PAGE_HEADER_SIZE = 8;

/**
 * @brief A slotted page storing opaque, variable-size rows (RowLayout bytes).
 *
 * ```
 *  | HEADER | slot array -> | ... free ... | <- row payloads |
 *  HEADER = NextPageId(4) NumTuples(2) NumDeletedTuples(2)
 *  slot i = offset(2) size(2) TupleMeta  (24 bytes)
 * ```
 *
 * The slot array grows forward from the header; row payloads grow backward from the end of the page.
 * Slot numbers are stable forever (deletion is logical via `TupleMeta::is_deleted_`), so a RID that
 * points at a slot stays valid. This class is an overlay reinterpreted onto a pinned page frame; it
 * never allocates and never knows the row's internal format — it just stores byte blobs.
 */
class TablePage {
 public:
  /** @brief Initialize an empty page header. */
  void Init();

  /** @return The number of slots (live or deleted) in this page. */
  auto GetNumTuples() const -> uint32_t { return num_tuples_; }

  /** @return The number of logically deleted slots. */
  auto GetNumDeletedTuples() const -> uint32_t { return num_deleted_tuples_; }

  auto GetNextPageId() const -> page_id_t { return next_page_id_; }
  void SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

  /**
   * @brief The offset a row of `row_size` bytes would occupy, or nullopt if it does not fit.
   *
   * The check rejects an oversized row *before* the unsigned subtraction can wrap (bug #1): a row
   * larger than the remaining space (or larger than the page) returns nullopt, never a bogus offset.
   */
  auto GetNextRowOffset(uint16_t row_size) const -> std::optional<uint16_t>;

  /**
   * @brief Copy a row's bytes into the page.
   *
   * @return The slot number, or nullopt if the row does not fit.
   */
  auto InsertRow(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size) -> std::optional<uint16_t>;

  /** @brief Replace a slot's metadata (e.g. to mark it deleted). */
  void UpdateTupleMeta(const TupleMeta &meta, uint16_t slot);

  /** @return `(meta, pointer-into-page, size)` for a slot. The pointer is valid while the page is pinned. */
  auto GetRow(uint16_t slot) const -> std::tuple<TupleMeta, const_data_ptr_t, uint16_t>;

  /** @return The metadata of a slot. */
  auto GetTupleMeta(uint16_t slot) const -> TupleMeta;

  /** @brief Overwrite a slot's row in place. The new row must be the same size as the old one. */
  void UpdateRowInPlaceUnsafe(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size, uint16_t slot);

  /**
   * @brief Overwrite a slot's row, allowing a different size by compacting the page in place.
   *
   * The slot number (and therefore the row's RID) is preserved — only the rows physically below the
   * slot shift, and their `tuple_info_` offsets are rewritten. A grow that would not fit the page's
   * free space is rejected. Same-size updates take the cheap in-place path.
   *
   * @return true on success; false if the (larger) row does not fit the page.
   */
  auto UpdateRow(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size, uint16_t slot) -> bool;

  static_assert(sizeof(page_id_t) == 4);

 private:
  using SlotInfo = std::tuple<uint16_t, uint16_t, TupleMeta>;  // (offset, size, meta)

  char page_start_[0];  // marker at page offset 0, so page_start_ + offset is absolute
  page_id_t next_page_id_;
  uint16_t num_tuples_;
  uint16_t num_deleted_tuples_;
  SlotInfo tuple_info_[0];  // flexible slot array, begins at offset TABLE_PAGE_HEADER_SIZE

  static constexpr size_t TUPLE_INFO_SIZE = 24;
  static_assert(sizeof(SlotInfo) == TUPLE_INFO_SIZE);
};

static_assert(sizeof(TablePage) == TABLE_PAGE_HEADER_SIZE);

}  // namespace bumblebee
