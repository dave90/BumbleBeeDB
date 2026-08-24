//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// result_writer.cpp
//
// Identification: src/main/result_writer.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/result_writer.h"

#include "main/query_result.h"

namespace bumblebee {

void RenderQueryResult(const QueryResult &result, ResultWriter &writer) {
  if (result.IsCommand()) {
    writer.OneCell(result.Status());
    return;
  }
  writer.BeginTable(true);
  writer.BeginHeader();
  for (const auto &column : result.Columns()) {
    writer.WriteHeaderCell(column);
  }
  writer.EndHeader();
  const idx_t limit = writer.MaxDisplayRows();
  idx_t shown = 0;
  for (const auto &chunk : result.Chunks()) {
    if (limit != 0 && shown >= limit) {
      break;
    }
    for (idx_t row_idx = 0; row_idx < chunk->GetSize(); row_idx++) {
      if (limit != 0 && shown >= limit) {
        break;
      }
      writer.BeginRow();
      for (idx_t column_idx = 0; column_idx < chunk->ColumnCount(); column_idx++) {
        writer.WriteCell(chunk->GetValue(column_idx, row_idx).ToString());
      }
      writer.EndRow();
      shown++;
    }
  }
  writer.EndTable();
  if (limit != 0 && result.RowCount() > limit) {
    writer.WriteTruncationNotice(shown, result.RowCount());
  }
}

}  // namespace bumblebee
