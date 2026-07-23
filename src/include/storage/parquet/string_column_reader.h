//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_column_reader.h
//
// Identification: src/include/storage/parquet/string_column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/parquet/templated_column_reader.h"
#include "type/bumble_string.h"

namespace bumblebee {

struct StringParquetValueConversion {
  static auto DictRead(ByteBuffer &dict, uint32_t &offset, ColumnReader &reader) -> string_t;
  static auto PlainRead(ByteBuffer &plain_data, ColumnReader &reader) -> string_t;
  static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader);
  static auto Null() -> string_t;
};

/**
 * @brief Reader for BYTE_ARRAY / FIXED_LEN_BYTE_ARRAY (string) columns. The produced string_t
 * values point zero-copy into the decompressed page / dictionary buffers; those buffers are
 * attached to the output Vector as auxiliary owners so the views never dangle.
 */
class StringColumnReader : public TemplatedColumnReader<string_t, StringParquetValueConversion> {
 public:
  StringColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t schema_idx,
                     idx_t max_define, idx_t max_repeat)
      : TemplatedColumnReader<string_t, StringParquetValueConversion>(reader, type_l, schema, schema_idx, max_define,
                                                                     max_repeat) {
    fixed_width_string_length_ = 0;
    if (schema.type == format::Type::FIXED_LEN_BYTE_ARRAY) {
      BUMBLEBEE_ASSERT(schema.__isset.type_length, "parquet invariant violated");
      fixed_width_string_length_ = schema.type_length;
    }
  }

  std::unique_ptr<string_t[]> dict_strings_;
  idx_t fixed_width_string_length_;

  void Dictionary(std::shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) override;

  /** @brief Validate UTF8; returns the possibly-truncated length (NULL bytes truncate). */
  auto VerifyString(const char *str_data, uint32_t str_len) -> uint32_t;

 protected:
  void DictReference(Vector &result) override;
  void PlainReference(std::shared_ptr<ByteBuffer> plain_data, Vector &result) override;
};

}  // namespace bumblebee
