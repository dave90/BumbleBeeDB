//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// list_entry.h
//
// Identification: src/include/type/list_entry.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"

namespace bumblebee {

/**
 * The per-row payload of a LIST: where this row's elements start in the child Vector,
 * and how many of them there are.
 *
 * A LIST Vector stores one ListEntry per row in its own data, and keeps the elements
 * themselves in a child Vector held by its ListDataMngr. Two rows are free to point at
 * overlapping ranges of the child — nothing forbids it — but every operation that
 * *writes* a list (SetValue, Copy, Append) appends a fresh range instead, so that
 * mutating one row can never disturb another.
 *
 * An ARRAY has no ListEntry at all: row i is simply the child slice
 * [i * array_size, (i + 1) * array_size).
 */
struct ListEntry {
  /** The index, in the child Vector, of the first element of this row. */
  idx_t offset_;
  /** The number of elements of this row. Zero for an empty list. */
  idx_t length_;
};

}  // namespace bumblebee
