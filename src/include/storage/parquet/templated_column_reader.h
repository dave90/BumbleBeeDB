//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// templated_column_reader.h
//
// Identification: src/include/storage/parquet/templated_column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <type_traits>

#include "common/limits.h"
#include "storage/parquet/column_reader.h"

namespace bumblebee {

template <class VALUE_TYPE>
struct TemplatedParquetValueConversion {
  static auto DictRead(ByteBuffer &dict, uint32_t &offset, ColumnReader &reader) -> VALUE_TYPE {
    BUMBLEBEE_ASSERT(offset < dict.len_ / sizeof(VALUE_TYPE), "parquet invariant violated");
    return reinterpret_cast<VALUE_TYPE *>(dict.ptr_)[offset];
  }

  static auto PlainRead(ByteBuffer &plain_data, ColumnReader &reader) -> VALUE_TYPE {
    return plain_data.Read<VALUE_TYPE>();
  }

  static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) { plain_data.Inc(sizeof(VALUE_TYPE)); }

  static auto Null() -> VALUE_TYPE { return NumericLimits<VALUE_TYPE>::Maximum(); }
};

/**
 * @brief True when one value's parquet payload is byte-identical to `VALUE_TYPE`, so a run of
 * defined, unfiltered values can be copied in bulk instead of decoded one at a time.
 *
 * The trivial conversion qualifies by construction (same type in and out). Any other conversion
 * opts in by defining a static `PlainIsBitwise(ColumnReader &)` — DECIMAL does, because its
 * INT32/INT64 storage already holds little-endian two's complement.
 *
 * @param reader The column reader (the storage layout is a property of its schema element).
 */
template <class VALUE_TYPE, class VALUE_CONVERSION>
auto PlainIsBitwise(ColumnReader &reader) -> bool {
  if constexpr (std::is_same_v<VALUE_CONVERSION, TemplatedParquetValueConversion<VALUE_TYPE>>) {
    return true;
  } else if constexpr (requires { VALUE_CONVERSION::PlainIsBitwise(reader); }) {
    return VALUE_CONVERSION::PlainIsBitwise(reader);
  } else {
    return false;
  }
}

/** @brief Column reader for fixed-width types whose parquet payload is (convertible to) the
 * in-memory representation. */
template <class VALUE_TYPE, class VALUE_CONVERSION>
class TemplatedColumnReader : public ColumnReader {
 public:
  TemplatedColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t schema_idx,
                        idx_t max_define, idx_t max_repeat)
      : ColumnReader(reader, type_l, schema, schema_idx, max_define, max_repeat) {}

  std::shared_ptr<ByteBuffer> dict_;

  void Dictionary(std::shared_ptr<ByteBuffer> data, idx_t num_entries) override { dict_ = std::move(data); }

  void Offsets(uint32_t *offsets, uint8_t *defines, idx_t num_values, parquet_filter_t &filter, idx_t result_offset,
               Vector &result) override {
    BUMBLEBEE_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR, "parquet invariant violated");
    auto *result_ptr = FlatVector::GetData<VALUE_TYPE>(result);

    if constexpr (std::is_same_v<VALUE_CONVERSION, TemplatedParquetValueConversion<VALUE_TYPE>>) {
      // Fast path for plain fixed-width dictionary values: when the batch has no NULLs and no
      // row is filtered out, the per-row loop below degenerates to a branch-free dictionary
      // gather. Only valid for the trivial conversion, whose DictRead is exactly dict[offset].
      if (filter.all() && AllDefined(defines, result_offset, num_values)) {
        auto *dict_ptr = reinterpret_cast<VALUE_TYPE *>(dict_->ptr_);
        for (idx_t i = 0; i < num_values; i++) {
          result_ptr[i + result_offset] = dict_ptr[offsets[i]];
        }
        return;
      }
    }

    idx_t offset_idx = 0;
    for (idx_t row_idx = 0; row_idx < num_values; row_idx++) {
      if (HasDefines() && defines[row_idx + result_offset] != max_define_) {
        // NULL: a definition level below the column max means the value is absent from the
        // payload. Clear the validity bit and write the sentinel as defensive fill (never
        // read once the bit is cleared).
        result.SetInvalid(row_idx + result_offset);
        result_ptr[row_idx + result_offset] = VALUE_CONVERSION::Null();
        continue;
      }
      if (filter[row_idx + result_offset]) {
        result_ptr[row_idx + result_offset] = VALUE_CONVERSION::DictRead(*dict_, offsets[offset_idx++], *this);
      } else {
        offset_idx++;
      }
    }
  }

  void Plain(std::shared_ptr<ByteBuffer> plain_data, uint8_t *defines, idx_t num_values, parquet_filter_t &filter,
             idx_t result_offset, Vector &result) override {
    auto *result_ptr = FlatVector::GetData<VALUE_TYPE>(result);

    // Fast path for bitwise-identical payloads: with no NULLs and no filtered rows, the per-row
    // decode loop is a straight buffer copy. CopyTo/Inc perform the same bounds check the
    // per-value reads would.
    if (filter.all() && PlainIsBitwise<VALUE_TYPE, VALUE_CONVERSION>(*this) &&
        AllDefined(defines, result_offset, num_values)) {
      const uint64_t bytes = num_values * sizeof(VALUE_TYPE);
      plain_data->CopyTo(reinterpret_cast<char *>(result_ptr + result_offset), bytes);
      plain_data->Inc(bytes);
      return;
    }

    for (idx_t row_idx = 0; row_idx < num_values; row_idx++) {
      if (HasDefines() && defines[row_idx + result_offset] != max_define_) {
        // NULL: see Offsets() above.
        result.SetInvalid(row_idx + result_offset);
        result_ptr[row_idx + result_offset] = VALUE_CONVERSION::Null();
        continue;
      }
      if (filter[row_idx + result_offset]) {
        result_ptr[row_idx + result_offset] = VALUE_CONVERSION::PlainRead(*plain_data, *this);
      } else {  // there is still some data there that we have to skip over
        VALUE_CONVERSION::PlainSkip(*plain_data, *this);
      }
    }
  }
};

}  // namespace bumblebee
