//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// schema.cpp
//
// Identification: src/catalog/schema.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "catalog/schema.h"

#include <sstream>

#include "fmt/ranges.h"

namespace bumblebee {

Schema::Schema(const std::vector<Column> &columns) {
  uint32_t curr_offset = 0;
  for (uint32_t index = 0; index < columns.size(); index++) {
    Column column = columns[index];
    if (!column.IsInlined()) {
      tuple_is_inlined_ = false;
      uninlined_columns_.push_back(index);
    }
    column.column_offset_ = curr_offset;
    if (column.IsInlined()) {
      curr_offset += column.GetStorageSize();
    } else {
      // An uninlined column stores an offset into the row's variable-length area.
      curr_offset += sizeof(uint32_t);
    }
    columns_.push_back(column);
  }
  length_ = curr_offset;
}

auto Schema::ToString(bool simplified) const -> std::string {
  std::vector<std::string> parts;
  parts.reserve(columns_.size());
  for (const auto &column : columns_) {
    parts.emplace_back(column.ToString(simplified));
  }
  if (simplified) {
    return fmt::format("({})", fmt::join(parts, ", "));
  }
  return fmt::format("Schema[NumColumns:{}, IsInlined:{}, Length:{}] :: ({})", columns_.size(),
                     tuple_is_inlined_, length_, fmt::join(parts, ", "));
}

}  // namespace bumblebee
