//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column.cpp
//
// Identification: src/catalog/column.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "catalog/column.h"

namespace bumblebee {

auto Column::ToString(bool simplified) const -> std::string {
  if (simplified) {
    return fmt::format("{}:{}", column_name_, column_type_.ToString());
  }
  return fmt::format("Column[{}, {}, offset:{}, length:{}]", column_name_, column_type_.ToString(),
                     column_offset_, length_);
}

}  // namespace bumblebee
