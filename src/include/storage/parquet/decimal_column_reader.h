//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// decimal_column_reader.h
//
// Identification: src/include/storage/parquet/decimal_column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/parquet/templated_column_reader.h"

namespace bumblebee {

template <class PHYSICAL_TYPE>
struct DecimalParquetValueConversion {
  /** @brief Payload width of one decimal value; `plain_decoder` selects the direct little-endian
   * path (INT32/INT64 storage) vs the big-endian two's-complement byte-array path. */
  static auto GetByteLength(ColumnReader &reader, bool &plain_decoder) -> idx_t {
    plain_decoder = false;
    switch (reader.GetSchema().type) {
      case format::Type::INT32:
        plain_decoder = true;
        return sizeof(int32_t);
      case format::Type::INT64:
        plain_decoder = true;
        return sizeof(int64_t);
      default: {
        auto len = static_cast<idx_t>(reader.GetSchema().type_length);
        if (len == 0) {
          throw NotImplementedException("Parquet Decimal type with byte length 0");
        }
        return len;
      }
    }
  }

  /** @brief INT32/INT64-stored decimals of exactly this width are already the in-memory value, so
   * a run of them copies in bulk (see `bumblebee::PlainIsBitwise`). */
  static auto PlainIsBitwise(ColumnReader &reader) -> bool {
    bool plain_decoder = false;
    const auto byte_len = GetByteLength(reader, plain_decoder);
    return plain_decoder && byte_len == sizeof(PHYSICAL_TYPE);
  }

  static auto DictRead(ByteBuffer &dict, uint32_t &offset, ColumnReader &reader) -> PHYSICAL_TYPE {
    auto *dict_ptr = reinterpret_cast<PHYSICAL_TYPE *>(dict.ptr_);
    return dict_ptr[offset];
  }

  static auto PlainRead(ByteBuffer &plain_data, ColumnReader &reader) -> PHYSICAL_TYPE {
    PHYSICAL_TYPE res = 0;
    bool plain_read;
    auto byte_len = GetByteLength(reader, plain_read);
    BUMBLEBEE_ASSERT(byte_len <= sizeof(PHYSICAL_TYPE), "parquet invariant violated");
    plain_data.Available(byte_len);
    if (plain_read) {
      // INT32/INT64 storage: the value is already little-endian two's complement.
      auto *direct = reinterpret_cast<PHYSICAL_TYPE *>(plain_data.ptr_);
      plain_data.Inc(byte_len);
      return *direct;
    }
    auto *res_ptr = reinterpret_cast<uint8_t *>(&res);

    // FIXED_LEN_BYTE_ARRAY storage: big-endian two's complement.
    bool positive = (*plain_data.ptr_ & 0x80) == 0;

    for (idx_t i = 0; i < byte_len; i++) {
      auto byte = static_cast<uint8_t>(*(plain_data.ptr_ + (byte_len - i - 1)));
      res_ptr[i] = positive ? byte : byte ^ 0xFF;
    }
    plain_data.Inc(byte_len);
    if (!positive) {
      res += 1;
      return -res;
    }
    return res;
  }

  static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
    bool plain_read;
    auto len = GetByteLength(reader, plain_read);
    plain_data.Inc(len);
  }

  static auto Null() -> PHYSICAL_TYPE { return NumericLimits<PHYSICAL_TYPE>::Maximum(); }
};

/** @brief Reader for DECIMAL columns; PHYSICAL_TYPE is the backing integer selected by width. */
template <class PHYSICAL_TYPE>
class DecimalColumnReader : public TemplatedColumnReader<PHYSICAL_TYPE, DecimalParquetValueConversion<PHYSICAL_TYPE>> {
 public:
  DecimalColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t file_idx,
                      idx_t max_define, idx_t max_repeat)
      : TemplatedColumnReader<PHYSICAL_TYPE, DecimalParquetValueConversion<PHYSICAL_TYPE>>(
            reader, type_l, schema, file_idx, max_define, max_repeat) {}

 protected:
  void Dictionary(std::shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) override {
    this->dict_ = std::make_shared<ResizeableBuffer>(this->reader_.allocator_, num_entries * sizeof(PHYSICAL_TYPE));
    auto *dict_ptr = reinterpret_cast<PHYSICAL_TYPE *>(this->dict_->ptr_);
    for (idx_t i = 0; i < num_entries; i++) {
      dict_ptr[i] = DecimalParquetValueConversion<PHYSICAL_TYPE>::PlainRead(*dictionary_data, *this);
    }
  }
};

}  // namespace bumblebee
