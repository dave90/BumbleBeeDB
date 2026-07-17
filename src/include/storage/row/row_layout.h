//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// row_layout.h
//
// Identification: src/include/storage/row/row_layout.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

#include "common/config.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * @brief The 8-byte, slot-relative handle a variable-length column occupies in a row's fixed region.
 *
 * `offset_` is the byte offset of the payload from the start of the row (the same page slot), so it
 * survives a flush and reload — no pointer swizzling. `length_` is the payload length in bytes.
 */
struct StringHandle {
  uint32_t offset_;
  uint32_t length_;
};
static_assert(sizeof(StringHandle) == 8, "StringHandle must be 8 bytes");

/**
 * @brief Describes how a row is laid out in a page slot.
 *
 * ```
 * row = [ validity prefix | fixed columns (incl. 8-byte varlen handles) | varlen payload... ]
 *        \-- flag_width_ --/\------------- fixed_width_ (aligned) -------/
 * ```
 *
 * The validity prefix holds one bit per column (bit = 1 means valid / not NULL). A constant-size
 * column occupies its physical size inline; a variable-length column occupies an 8-byte `StringHandle`
 * whose payload is appended after the fixed region within the same slot. A row's total size is
 * therefore `fixed_width_ + Σ payload lengths`, which varies per row.
 */
class RowLayout {
 public:
  RowLayout() = default;

  /** @brief Compute the offsets and widths for a row of these column types. */
  void Initialize(std::vector<LogicalType> types);

  auto GetColumnCount() const -> idx_t { return types_.size(); }
  auto GetTypes() const -> const std::vector<LogicalType> & { return types_; }
  /** @return The byte offset of each column within a row's fixed region. */
  auto GetOffsets() const -> const std::vector<idx_t> & { return offsets_; }
  /** @return The width of the per-row validity prefix, in bytes. */
  auto GetFlagWidth() const -> idx_t { return flag_width_; }
  /** @return The width of the validity prefix plus all fixed columns, aligned. */
  auto GetFixedRowWidth() const -> idx_t { return fixed_width_; }
  /** @return True if every column is fixed size (no varlen payloads). */
  auto AllConstant() const -> bool { return all_constant_; }

 private:
  std::vector<LogicalType> types_;
  std::vector<idx_t> offsets_;
  idx_t flag_width_{0};
  idx_t fixed_width_{0};
  bool all_constant_{true};
};

}  // namespace bumblebee
