//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// row_layout.cpp
//
// Identification: src/storage/row/row_layout.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/row/row_layout.h"

#include <utility>

namespace bumblebee {

/** @brief Round up to the next multiple of 8, so the next row starts aligned. */
static constexpr auto AlignValue(idx_t n) -> idx_t { return (n + 7) & ~idx_t(7); }

void RowLayout::Initialize(std::vector<LogicalType> types) {
  types_ = std::move(types);
  offsets_.clear();

  // One validity bit per column, at row offset 0. Empty layouts carry no prefix.
  flag_width_ = types_.empty() ? 0 : (types_.size() + 7) / 8;
  idx_t row_width = flag_width_;

  all_constant_ = true;
  for (const auto &type : types_) {
    all_constant_ = all_constant_ && type.IsConstantSize();
  }

  for (const auto &type : types_) {
    offsets_.push_back(row_width);
    if (type.IsConstantSize()) {
      row_width += LogicalType::SizeOf(type.GetPhysicalType());
    } else {
      // A variable-length column occupies an 8-byte handle; its payload lives after the fixed region.
      row_width += sizeof(StringHandle);
    }
  }

  fixed_width_ = AlignValue(row_width);
}

}  // namespace bumblebee
