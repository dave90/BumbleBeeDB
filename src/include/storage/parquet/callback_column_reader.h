//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// callback_column_reader.h
//
// Identification: src/include/storage/parquet/callback_column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/parquet/templated_column_reader.h"

namespace bumblebee {

template <class PARQUET_PHYSICAL_TYPE, class PHYSICAL_TYPE, PHYSICAL_TYPE (*FUNC)(const PARQUET_PHYSICAL_TYPE &input)>
struct CallbackParquetValueConversion {
  static auto DictRead(ByteBuffer &dict, uint32_t &offset, ColumnReader &reader) -> PHYSICAL_TYPE {
    return TemplatedParquetValueConversion<PHYSICAL_TYPE>::DictRead(dict, offset, reader);
  }

  static auto PlainRead(ByteBuffer &plain_data, ColumnReader &reader) -> PHYSICAL_TYPE {
    return FUNC(plain_data.Read<PARQUET_PHYSICAL_TYPE>());
  }

  static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
    plain_data.Inc(sizeof(PARQUET_PHYSICAL_TYPE));
  }

  static auto Null() -> PHYSICAL_TYPE { return NumericLimits<PHYSICAL_TYPE>::Maximum(); }
};

/**
 * @brief Reader for columns whose parquet payload type differs from the in-memory type and is
 * converted value-by-value through FUNC (e.g. INT96 -> timestamp_t). The dictionary is
 * materialized already-converted, so DictRead stays a plain array lookup.
 */
template <class PARQUET_PHYSICAL_TYPE, class PHYSICAL_TYPE, PHYSICAL_TYPE (*FUNC)(const PARQUET_PHYSICAL_TYPE &input)>
class CallbackColumnReader
    : public TemplatedColumnReader<PHYSICAL_TYPE,
                                   CallbackParquetValueConversion<PARQUET_PHYSICAL_TYPE, PHYSICAL_TYPE, FUNC>> {
 public:
  CallbackColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t file_idx,
                       idx_t max_define, idx_t max_repeat)
      : TemplatedColumnReader<PHYSICAL_TYPE, CallbackParquetValueConversion<PARQUET_PHYSICAL_TYPE, PHYSICAL_TYPE, FUNC>>(
            reader, type_l, schema, file_idx, max_define, max_repeat) {}

 protected:
  void Dictionary(std::shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) override {
    this->dict_ = std::make_shared<ResizeableBuffer>(this->reader_.allocator_, num_entries * sizeof(PHYSICAL_TYPE));
    auto *dict_ptr = reinterpret_cast<PHYSICAL_TYPE *>(this->dict_->ptr_);
    for (idx_t i = 0; i < num_entries; i++) {
      dict_ptr[i] = FUNC(dictionary_data->Read<PARQUET_PHYSICAL_TYPE>());
    }
  }
};

}  // namespace bumblebee
