//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// schema_codec.h
//
// Identification: src/include/catalog/schema_codec.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/byte_buffer.h"
#include "type/logical_type.h"

namespace bumblebee {

/** @brief Encode a LogicalType: its id, plus the extra data DECIMAL / LIST / ARRAY carry. */
inline void SerializeLogicalType(ByteWriter &w, const LogicalType &type) {
  w.PutU16(static_cast<uint16_t>(type.GetTypeId()));
  switch (type.GetTypeId()) {
    case LogicalTypeId::DECIMAL: {
      const auto &d = type.GetDecimalData();
      w.PutI32(d.width_);
      w.PutI32(d.scale_);
      break;
    }
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY: {
      const auto &l = type.GetListData();
      w.PutU64(l.size_);
      SerializeLogicalType(w, *l.child_type_);  // recurse into the element type
      break;
    }
    default:
      break;  // a plain scalar carries no extra data
  }
}

/** @brief Decode a LogicalType written by SerializeLogicalType, rebuilding via the type factories. */
inline auto DeserializeLogicalType(ByteReader &r) -> LogicalType {
  auto id = static_cast<LogicalTypeId>(r.GetU16());
  switch (id) {
    case LogicalTypeId::DECIMAL: {
      auto width = r.GetI32();
      auto scale = r.GetI32();
      return LogicalType::Decimal(width, scale);
    }
    case LogicalTypeId::LIST: {
      auto size = static_cast<idx_t>(r.GetU64());
      (void)size;  // a LIST is variable-length
      return LogicalType::List(DeserializeLogicalType(r));
    }
    case LogicalTypeId::ARRAY: {
      auto size = static_cast<idx_t>(r.GetU64());
      return LogicalType::Array(DeserializeLogicalType(r), size);
    }
    default:
      return LogicalType(id);
  }
}

/**
 * @brief Encode a Schema: per column, its name, type, and inline storage width (needed to rebuild a
 * varlen column; ignored for fixed-width columns).
 */
inline void SerializeSchema(ByteWriter &w, const Schema &schema) {
  w.PutU32(static_cast<uint32_t>(schema.GetColumnCount()));
  for (const auto &col : schema.GetColumns()) {
    w.PutString(col.GetName());
    SerializeLogicalType(w, col.GetType());
    w.PutU32(col.GetStorageSize());
  }
}

/**
 * @brief Decode a Schema. The `Schema(columns)` constructor recomputes offsets / total width /
 * uninlined columns, so only the column list has to be reconstructed.
 */
inline auto DeserializeSchema(ByteReader &r) -> Schema {
  auto count = r.GetU32();
  std::vector<Column> cols;
  cols.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    auto name = r.GetString();
    auto type = DeserializeLogicalType(r);
    auto length = r.GetU32();
    // The 3-arg Column ctor works for every type: a fixed-width type ignores `length` and uses its
    // physical size, a varlen type keeps the declared width.
    cols.emplace_back(std::move(name), type, length);
  }
  return Schema{cols};
}

}  // namespace bumblebee
