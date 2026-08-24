//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// query_result.cpp
//
// Identification: src/main/query_result.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/query_result.h"

#include <utility>

namespace bumblebee {

static auto IsDml(StatementType type) -> bool {
  return type == StatementType::INSERT_STATEMENT || type == StatementType::UPDATE_STATEMENT ||
         type == StatementType::DELETE_STATEMENT;
}

auto QueryResult::Rows(const Schema &schema, data_chunk_vector_t chunks, StatementType statement_type) -> QueryResult {
  QueryResult result;
  result.kind_ = QueryResultKind::ROWS;
  result.statement_type_ = statement_type;
  result.columns_.reserve(schema.GetColumnCount());
  result.types_.reserve(schema.GetColumnCount());
  for (const auto &column : schema.GetColumns()) {
    result.columns_.push_back(column.GetName());
    result.types_.push_back(column.GetType());
  }
  for (const auto &chunk : chunks) {
    result.row_count_ += chunk->GetSize();
  }
  result.chunks_ = std::move(chunks);

  if (IsDml(statement_type) && result.row_count_ == 1 && !result.chunks_.empty() &&
      result.chunks_.front()->ColumnCount() == 1) {
    result.affected_rows_ = result.chunks_.front()->GetValue(0, 0).GetAs<int64_t>();
    result.command_tag_ = StatementTypeToString(statement_type);
  }
  return result;
}

auto QueryResult::Command(std::string command_tag, std::string status, std::optional<int64_t> affected_rows,
                          StatementType statement_type) -> QueryResult {
  QueryResult result;
  result.kind_ = QueryResultKind::COMMAND;
  result.statement_type_ = statement_type;
  result.columns_.push_back("status");
  result.types_.emplace_back(LogicalTypeId::STRING);
  auto chunk = std::make_unique<DataChunk>();
  chunk->Initialize(result.types_);
  chunk->SetValue(0, 0, Value(status));
  chunk->SetCardinality(1);
  result.chunks_.push_back(std::move(chunk));
  result.row_count_ = 1;
  result.command_tag_ = std::move(command_tag);
  result.status_ = std::move(status);
  result.affected_rows_ = affected_rows;
  return result;
}

auto QueryResult::MaterializeRows() const -> std::vector<std::vector<Value>> {
  std::vector<std::vector<Value>> rows;
  rows.reserve(row_count_);
  for (const auto &chunk : chunks_) {
    for (idx_t row_idx = 0; row_idx < chunk->GetSize(); row_idx++) {
      std::vector<Value> row;
      row.reserve(chunk->ColumnCount());
      for (idx_t column_idx = 0; column_idx < chunk->ColumnCount(); column_idx++) {
        row.push_back(chunk->GetValue(column_idx, row_idx));
      }
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

}  // namespace bumblebee
