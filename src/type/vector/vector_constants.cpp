//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_constants.cpp
//
// Identification: src/type/vector/vector_constants.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <array>

#include "common/config.h"
#include "type/vector/selection_vector.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** @brief Build the identity table 0..STANDARD_VECTOR_SIZE-1 at compile time, for any vector size. */
static constexpr auto MakeIncrementalVector() -> std::array<sel_t, STANDARD_VECTOR_SIZE> {
  std::array<sel_t, STANDARD_VECTOR_SIZE> table{};
  for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
    table[i] = static_cast<sel_t>(i);
  }
  return table;
}

// The two selections every kernel falls back on:
//
//  - ZERO_SELECTION_VECTOR reads row 0 whatever the index: that is how a CONSTANT_VECTOR
//    is read through the same (data, sel) loop as any other vector.
//  - INCREMENTAL_SELECTION_VECTOR is the identity. It is deliberately DEFAULT-constructed
//    (a null index array), so GetIndex(i) returns i with no memory touched at all;
//    INCREMENTAL_VECTOR below is the materialized form, for the callers that need a
//    real sel_t array.
const SelectionVector ConstantVector::ZERO_SELECTION_VECTOR =
    SelectionVector(const_cast<sel_t *>(ConstantVector::ZERO_VECTOR));
const SelectionVector FlatVector::INCREMENTAL_SELECTION_VECTOR;

const sel_t ConstantVector::ZERO_VECTOR[STANDARD_VECTOR_SIZE] = {0};

// The identity table 0..STANDARD_VECTOR_SIZE-1, generated at compile time so it scales to whatever
// STANDARD_VECTOR_SIZE the build was configured with (see BBDB_VECTOR_SIZE / the small-vector test build).
const std::array<sel_t, STANDARD_VECTOR_SIZE> FlatVector::INCREMENTAL_VECTOR = MakeIncrementalVector();

}  // namespace bumblebee
