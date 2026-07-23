//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// boolean_column_reader.h
//
// Identification: src/include/storage/parquet/boolean_column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/parquet/templated_column_reader.h"

namespace bumblebee {

struct BooleanParquetValueConversion;

/** @brief Reader for BOOLEAN columns (bit-packed plain encoding; physical uint8 in-memory). */
class BooleanColumnReader : public TemplatedColumnReader<uint8_t, BooleanParquetValueConversion> {
 public:
  BooleanColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t schema_idx,
                      idx_t max_define, idx_t max_repeat)
      : TemplatedColumnReader<uint8_t, BooleanParquetValueConversion>(reader, type_l, schema, schema_idx, max_define,
                                                                     max_repeat),
        byte_pos_(0) {}

  uint8_t byte_pos_;

  void InitializeRead(const std::vector<ColumnChunk> &columns, TProtocol &protocol) override {
    byte_pos_ = 0;
    TemplatedColumnReader<uint8_t, BooleanParquetValueConversion>::InitializeRead(columns, protocol);
  }
};

struct BooleanParquetValueConversion {
  static auto DictRead(ByteBuffer &dict, uint32_t &offset, ColumnReader &reader) -> uint8_t {
    throw NotImplementedException("Dicts for booleans make no sense");
  }

  static auto PlainRead(ByteBuffer &plain_data, ColumnReader &reader) -> uint8_t {
    plain_data.Available(1);
    auto &byte_pos = static_cast<BooleanColumnReader &>(reader).byte_pos_;
    bool ret = (*plain_data.ptr_ >> byte_pos) & 1;
    byte_pos++;
    if (byte_pos == 8) {
      byte_pos = 0;
      plain_data.Inc(1);
    }
    return ret ? 1 : 0;
  }

  static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) { PlainRead(plain_data, reader); }

  static auto Null() -> uint8_t { return NumericLimits<uint8_t>::Maximum(); }
};

}  // namespace bumblebee
