//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// selection_vector.cpp
//
// Identification: src/type/vector/selection_vector.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/selection_vector.h"

namespace bumblebee {

auto SelectionVector::Slice(const SelectionVector &sel, idx_t count) const -> sel_ptr_t {
  auto data = sel_ptr_t(new sel_t[count]);
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel.GetIndex(i);
    data[i] = static_cast<sel_t>(this->GetIndex(idx));
  }
  return data;
}

auto SelectionVector::ToString(idx_t count) const -> std::string {
  if (!sel_data_) {
    return "";
  }
  std::string result;
  for (idx_t i = 0; i < count; i++) {
    result += std::to_string(sel_vector_[i]) + ", ";
  }
  return result;
}

}  // namespace bumblebee
