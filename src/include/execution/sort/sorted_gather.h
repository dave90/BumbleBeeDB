//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// sorted_gather.h
//
// Identification: src/include/execution/sort/sorted_gather.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"
#include "type/bumble_string.h"
#include "type/vector/chunk_collection.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/** One row in sort order: its encoded ORDER BY key and its row index into the input collection. */
struct SortEntry {
  string_t key_;
  idx_t row_;
};

/**
 * @brief Gather `count` rows of `source`, in the order of `entries`, into `out`.
 *
 * The vectorized emit of a sort: `entries[i].row_` becomes row i of `out`, copied with one
 * batched selection copy per (source chunk, column) — no per-cell Value traffic.
 *
 * @param source The collection the entries index into.
 * @param entries The rows to gather, in output order.
 * @param count The number of rows. At most STANDARD_VECTOR_SIZE.
 * @param out The chunk to write into. Rows land at [0, count); the caller sets the cardinality.
 * @param col_offset The first `out` column to write: source column c lands in `col_offset + c`.
 */
void GatherSorted(ChunkCollection &source, const SortEntry *entries, idx_t count, DataChunk &out,
                  idx_t col_offset = 0);

}  // namespace bumblebee
