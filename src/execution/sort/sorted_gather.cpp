//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// sorted_gather.cpp
//
// Identification: src/execution/sort/sorted_gather.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/sort/sorted_gather.h"

#include <unordered_map>
#include <utility>
#include <vector>

#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

void GatherSorted(ChunkCollection &source, const SortEntry *entries, idx_t count, DataChunk &out,
                  idx_t col_offset) {
  BUMBLEBEE_ASSERT(count <= STANDARD_VECTOR_SIZE, "GatherSorted: more rows than one chunk holds");

  // Bucket the rows by the source chunk they live in (row r lives in chunk r / VECTOR_SIZE).
  std::unordered_map<idx_t, std::pair<std::vector<sel_t>, std::vector<sel_t>>> groups;
  for (idx_t i = 0; i < count; i++) {
    const idx_t row = entries[i].row_;
    auto &[src_rows, tgt_rows] = groups[row / STANDARD_VECTOR_SIZE];
    src_rows.push_back(static_cast<sel_t>(row % STANDARD_VECTOR_SIZE));
    tgt_rows.push_back(static_cast<sel_t>(i));
  }

  const idx_t ncols = source.ColumnCount();
  for (auto &[ci, group] : groups) {
    SelectionVector src_sel(group.first.data());
    SelectionVector tgt_sel(group.second.data());
    auto &src_chunk = source.GetChunk(ci);
    for (idx_t c = 0; c < ncols; c++) {
      VectorOperations::Copy(src_chunk.data_[c], out.data_[col_offset + c], src_sel, &tgt_sel, group.first.size(), 0,
                             0);
    }
  }
}

}  // namespace bumblebee
