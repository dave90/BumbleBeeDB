//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// struct_column_reader.h
//
// Identification: src/include/storage/parquet/struct_column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <utility>

#include "storage/parquet/column_reader.h"

namespace bumblebee {

template <class T>
using child_list_t = std::vector<std::pair<std::string, T>>;

/**
 * @brief Reader for the root schema node: fans InitializeRead out to one child reader per leaf
 * column. Only the root may be a struct (nested structs are rejected at schema derivation).
 */
class StructColumnReader : public ColumnReader {
 public:
  StructColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t schema_idx,
                     idx_t max_define, idx_t max_repeat, std::vector<std::unique_ptr<ColumnReader>> child_readers,
                     child_list_t<LogicalType> child_types)
      : ColumnReader(reader, type_l, schema, schema_idx, max_define, max_repeat),
        child_readers_(std::move(child_readers)),
        child_types_(std::move(child_types)) {
    BUMBLEBEE_ASSERT(!child_readers_.empty(), "parquet invariant violated");
  }

  auto GetChildReader(idx_t child_idx) -> ColumnReader * { return child_readers_[child_idx].get(); }

  void InitializeRead(const std::vector<ColumnChunk> &columns, TProtocol &protocol) override {
    for (auto &child : child_readers_) {
      child->InitializeRead(columns, protocol);
    }
  }

  auto GroupRowsAvailable() -> idx_t override { return child_readers_[0]->GroupRowsAvailable(); }

  std::vector<std::unique_ptr<ColumnReader>> child_readers_;
  child_list_t<LogicalType> child_types_;
};

}  // namespace bumblebee
