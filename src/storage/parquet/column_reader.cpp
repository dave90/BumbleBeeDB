//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column_reader.cpp
//
// Identification: src/storage/parquet/column_reader.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/column_reader.h"

#include <sstream>

#include "miniz/miniz_wrapper.hpp"
#include "storage/parquet/boolean_column_reader.h"
#include "storage/parquet/callback_column_reader.h"
#include "storage/parquet/decimal_column_reader.h"
#include "storage/parquet/parquet_reader.h"
#include "storage/parquet/parquet_timestamp.h"
#include "storage/parquet/string_column_reader.h"
#include "storage/parquet/templated_column_reader.h"
#include "snappy/snappy.h"
#include "zstd/include/zstd.h"

namespace bumblebee {

using format::CompressionCodec;
using format::ConvertedType;
using format::Encoding;
using format::PageType;
using format::Type;

const uint32_t RleBpDecoder::BITPACK_MASKS[] = {
    0,       1,       3,        7,        15,       31,        63,        127,       255,        511,       1023,
    2047,    4095,    8191,     16383,    32767,    65535,     131071,    262143,    524287,     1048575,   2097151,
    4194303, 8388607, 16777215, 33554431, 67108863, 134217727, 268435455, 536870911, 1073741823, 2147483647};

const uint8_t RleBpDecoder::BITPACK_DLEN = 8;

ColumnReader::ColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t file_idx,
                           idx_t max_define, idx_t max_repeat)
    : schema_(schema),
      file_idx_(file_idx),
      max_define_(max_define),
      max_repeat_(max_repeat),
      reader_(reader),
      logical_type_(std::move(type_l)),
      page_rows_available_(0) {
  // Dummies for Skip().
  none_filter_.none();
  dummy_define_.Resize(reader_.allocator_, STANDARD_VECTOR_SIZE);
  dummy_repeat_.Resize(reader_.allocator_, STANDARD_VECTOR_SIZE);
}

ColumnReader::~ColumnReader() = default;

auto ColumnReader::CreateReader(ParquetReader &reader, const LogicalType &type_l, const SchemaElement &schema,
                                idx_t file_idx, idx_t max_define, idx_t max_repeat) -> std::unique_ptr<ColumnReader> {
  switch (type_l.GetTypeId()) {
    case LogicalTypeId::BOOLEAN:
      return std::make_unique<BooleanColumnReader>(reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::DECIMAL:
      // The backing integer is selected by the decimal's declared width.
      switch (type_l.GetPhysicalType()) {
        case PhysicalType::SMALLINT:
          return std::make_unique<DecimalColumnReader<int16_t>>(reader, type_l, schema, file_idx, max_define,
                                                                max_repeat);
        case PhysicalType::INTEGER:
          return std::make_unique<DecimalColumnReader<int32_t>>(reader, type_l, schema, file_idx, max_define,
                                                                max_repeat);
        case PhysicalType::BIGINT:
          return std::make_unique<DecimalColumnReader<int64_t>>(reader, type_l, schema, file_idx, max_define,
                                                                max_repeat);
        default:
          throw NotImplementedException("DECIMAL logical type does not have a proper physical type");
      }
    case LogicalTypeId::DATE:
      return std::make_unique<CallbackColumnReader<int32_t, date_t, ParquetIntToDate>>(reader, type_l, schema,
                                                                                       file_idx, max_define,
                                                                                       max_repeat);
    case LogicalTypeId::TIMESTAMP:
      switch (schema.type) {
        case Type::INT96:
          return std::make_unique<CallbackColumnReader<Int96, timestamp_t, ImpalaTimestampToTimestamp>>(
              reader, type_l, schema, file_idx, max_define, max_repeat);
        case Type::INT64:
          switch (schema.converted_type) {
            case ConvertedType::TIMESTAMP_MICROS:
              return std::make_unique<CallbackColumnReader<int64_t, timestamp_t, ParquetTimestampMicrosToTimestamp>>(
                  reader, type_l, schema, file_idx, max_define, max_repeat);
            case ConvertedType::TIMESTAMP_MILLIS:
              return std::make_unique<CallbackColumnReader<int64_t, timestamp_t, ParquetTimestampMsToTimestamp>>(
                  reader, type_l, schema, file_idx, max_define, max_repeat);
            default:
              break;
          }
          break;
        default:
          break;
      }
      break;
    case LogicalTypeId::UTINYINT:
      return std::make_unique<TemplatedColumnReader<uint8_t, TemplatedParquetValueConversion<uint32_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::USMALLINT:
      return std::make_unique<TemplatedColumnReader<uint16_t, TemplatedParquetValueConversion<uint32_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::UINTEGER:
      return std::make_unique<TemplatedColumnReader<uint32_t, TemplatedParquetValueConversion<uint32_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      return std::make_unique<TemplatedColumnReader<uint64_t, TemplatedParquetValueConversion<uint64_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::TINYINT:
      return std::make_unique<TemplatedColumnReader<int8_t, TemplatedParquetValueConversion<int32_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::SMALLINT:
      return std::make_unique<TemplatedColumnReader<int16_t, TemplatedParquetValueConversion<int32_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::INTEGER:
      return std::make_unique<TemplatedColumnReader<int32_t, TemplatedParquetValueConversion<int32_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::BIGINT:
      return std::make_unique<TemplatedColumnReader<int64_t, TemplatedParquetValueConversion<int64_t>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::FLOAT:
      return std::make_unique<TemplatedColumnReader<float, TemplatedParquetValueConversion<float>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::DOUBLE:
      return std::make_unique<TemplatedColumnReader<double, TemplatedParquetValueConversion<double>>>(
          reader, type_l, schema, file_idx, max_define, max_repeat);
    case LogicalTypeId::STRING:
      return std::make_unique<StringColumnReader>(reader, type_l, schema, file_idx, max_define, max_repeat);
    default:
      break;
  }
  throw NotImplementedException(fmt::format("Unsupported parquet column type {}", type_l.ToString()));
}

void ColumnReader::PrepareRead(parquet_filter_t &filter) {
  dict_decoder_.reset();
  defined_decoder_.reset();
  block_.reset();

  PageHeader page_hdr;
  page_hdr.read(protocol_);

  // Dictionary pages are moved into the dictionary and live for the whole row group, so they
  // are allocated fresh; only per-page data buffers are drawn from the pool.
  const bool poolable = page_hdr.type != PageType::DICTIONARY_PAGE;
  PreparePage(page_hdr.compressed_page_size, page_hdr.uncompressed_page_size, poolable);

  switch (page_hdr.type) {
    case PageType::DATA_PAGE_V2:
    case PageType::DATA_PAGE:
      PrepareDataPage(page_hdr);
      break;
    case PageType::DICTIONARY_PAGE:
      Dictionary(std::move(block_), page_hdr.dictionary_page_header.num_values);
      break;
    default:
      break;  // ignore INDEX page type and any other custom extensions
  }
}

auto ColumnReader::AcquireDecompressed(idx_t size) -> std::shared_ptr<ResizeableBuffer> {
  // Reuse a pooled buffer no longer referenced by any live output chunk. block_ has been reset
  // by the time this runs, so use_count == 1 means only the pool entry holds it and no
  // zero-copy reference into it is still alive.
  for (auto &buf : decompressed_pool_) {
    if (buf.use_count() == 1) {
      buf->Resize(reader_.allocator_, size);
      return buf;
    }
  }
  auto buf = std::make_shared<ResizeableBuffer>(reader_.allocator_, size);
  decompressed_pool_.push_back(buf);
  return buf;
}

void ColumnReader::PreparePage(idx_t compressed_page_size, idx_t uncompressed_page_size, bool poolable) {
  auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol_->getTransport());

  // Decompressing every page into a freshly allocated buffer faults in a new OS page every
  // 4 KiB of output; reuse buffers across pages so their pages stay resident:
  //  - the compressed read buffer is pure transient input to the decompressor (never
  //    referenced afterwards) so it is always reused;
  //  - decompressed data-page buffers are referenced zero-copy by string columns, so they are
  //    drawn from a pool that hands back only buffers no live chunk still holds; dictionary
  //    pages (poolable == false) live for the whole row group and are allocated fresh.
  if (chunk_->meta_data.codec == CompressionCodec::UNCOMPRESSED) {
    // Uncompressed: the read buffer is the data buffer (referenced).
    block_ = poolable ? AcquireDecompressed(compressed_page_size + 1)
                      : std::make_shared<ResizeableBuffer>(reader_.allocator_, compressed_page_size + 1);
    trans.read(reinterpret_cast<uint8_t *>(block_->ptr_), compressed_page_size);
    return;
  }

  compressed_buffer_.Resize(reader_.allocator_, compressed_page_size + 1);
  trans.read(reinterpret_cast<uint8_t *>(compressed_buffer_.ptr_), compressed_page_size);

  std::shared_ptr<ResizeableBuffer> unpacked_block =
      poolable ? AcquireDecompressed(uncompressed_page_size + 1)
               : std::make_shared<ResizeableBuffer>(reader_.allocator_, uncompressed_page_size + 1);

  switch (chunk_->meta_data.codec) {
    case CompressionCodec::GZIP: {
      MiniZStream s;
      s.decompress(compressed_buffer_.ptr_, compressed_page_size, unpacked_block->ptr_, uncompressed_page_size);
      block_ = std::move(unpacked_block);
      break;
    }
    case CompressionCodec::SNAPPY: {
      auto res = snappy::RawUncompress(compressed_buffer_.ptr_, compressed_page_size, unpacked_block->ptr_);
      if (!res) {
        throw Exception("Snappy decompression failure");
      }
      block_ = std::move(unpacked_block);
      break;
    }
    case CompressionCodec::ZSTD: {
      auto res = ZSTD_decompress(unpacked_block->ptr_, uncompressed_page_size, compressed_buffer_.ptr_,
                                 compressed_page_size);
      if (ZSTD_isError(res) || res != static_cast<size_t>(uncompressed_page_size)) {
        throw Exception("ZSTD decompression failure");
      }
      block_ = std::move(unpacked_block);
      break;
    }
    default: {
      std::stringstream codec_name;
      codec_name << chunk_->meta_data.codec;
      throw NotImplementedException("Unsupported compression codec \"" + codec_name.str() +
                                    "\". Supported options are uncompressed, gzip, snappy or zstd");
    }
  }
}

static auto ComputeBitWidth(idx_t val) -> uint8_t {
  if (val == 0) {
    return 0;
  }
  uint8_t ret = 1;
  while ((static_cast<idx_t>(1u << ret) - 1) < val) {
    ret++;
  }
  return ret;
}

void ColumnReader::PrepareDataPage(PageHeader &page_hdr) {
  if (page_hdr.type == PageType::DATA_PAGE && !page_hdr.__isset.data_page_header) {
    throw Exception("Missing data page header from data page");
  }
  if (page_hdr.type == PageType::DATA_PAGE_V2 && !page_hdr.__isset.data_page_header_v2) {
    throw Exception("Missing data page header from data page v2");
  }

  page_rows_available_ = page_hdr.type == PageType::DATA_PAGE ? page_hdr.data_page_header.num_values
                                                              : page_hdr.data_page_header_v2.num_values;
  auto page_encoding = page_hdr.type == PageType::DATA_PAGE ? page_hdr.data_page_header.encoding
                                                            : page_hdr.data_page_header_v2.encoding;

  if (HasRepeats()) {
    uint32_t rep_length = page_hdr.type == PageType::DATA_PAGE
                              ? block_->Read<uint32_t>()
                              : page_hdr.data_page_header_v2.repetition_levels_byte_length;
    block_->Available(rep_length);
    repeated_decoder_ = std::make_unique<RleBpDecoder>(reinterpret_cast<const uint8_t *>(block_->ptr_), rep_length,
                                                       ComputeBitWidth(max_repeat_));
    block_->Inc(rep_length);
  }

  if (HasDefines()) {
    uint32_t def_length = page_hdr.type == PageType::DATA_PAGE
                              ? block_->Read<uint32_t>()
                              : page_hdr.data_page_header_v2.definition_levels_byte_length;
    block_->Available(def_length);
    defined_decoder_ = std::make_unique<RleBpDecoder>(reinterpret_cast<const uint8_t *>(block_->ptr_), def_length,
                                                      ComputeBitWidth(max_define_));
    block_->Inc(def_length);
  }

  switch (page_encoding) {
    case Encoding::RLE_DICTIONARY:
    case Encoding::PLAIN_DICTIONARY: {
      auto dict_width = block_->Read<uint8_t>();
      dict_decoder_ =
          std::make_unique<RleBpDecoder>(reinterpret_cast<const uint8_t *>(block_->ptr_), block_->len_, dict_width);
      block_->Inc(block_->len_);
      break;
    }
    case Encoding::PLAIN:
      // Nothing to do here, the payload will be read directly.
      break;
    default:
      throw NotImplementedException("Unsupported page encoding");
  }
}

auto ColumnReader::Read(uint64_t num_values, parquet_filter_t &filter, uint8_t *define_out, uint8_t *repeat_out,
                        Vector &result) -> idx_t {
  // We need to reset the location because multiple column readers share the same protocol.
  auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol_->getTransport());
  trans.SetLocation(chunk_read_offset_);

  // Reset the reused result so stale NULL bits don't leak.
  result.Validity().SetAllValid();

  idx_t result_offset = 0;
  auto to_read = num_values;

  while (to_read > 0) {
    while (page_rows_available_ == 0) {
      PrepareRead(filter);
    }

    BUMBLEBEE_ASSERT(block_, "parquet invariant violated");
    auto read_now = MinValue<idx_t>(to_read, page_rows_available_);

    BUMBLEBEE_ASSERT(read_now <= STANDARD_VECTOR_SIZE, "parquet invariant violated");

    if (HasRepeats()) {
      BUMBLEBEE_ASSERT(repeated_decoder_, "parquet invariant violated");
      repeated_decoder_->GetBatch<uint8_t>(reinterpret_cast<char *>(repeat_out) + result_offset, read_now);
    }

    if (HasDefines()) {
      BUMBLEBEE_ASSERT(defined_decoder_, "parquet invariant violated");
      defined_decoder_->GetBatch<uint8_t>(reinterpret_cast<char *>(define_out) + result_offset, read_now);
    }

    if (dict_decoder_) {
      // We need the null count because the offsets and plain values have no entries for nulls.
      idx_t null_count = 0;
      if (HasDefines()) {
        for (idx_t i = 0; i < read_now; i++) {
          if (define_out[i + result_offset] != max_define_) {
            null_count++;
          }
        }
      }

      offset_buffer_.Resize(reader_.allocator_, sizeof(uint32_t) * (read_now - null_count));
      dict_decoder_->GetBatch<uint32_t>(offset_buffer_.ptr_, read_now - null_count);
      DictReference(result);
      Offsets(reinterpret_cast<uint32_t *>(offset_buffer_.ptr_), define_out, read_now, filter, result_offset, result);
    } else {
      PlainReference(block_, result);
      Plain(block_, define_out, read_now, filter, result_offset, result);
    }

    result_offset += read_now;
    page_rows_available_ -= read_now;
    to_read -= read_now;
  }
  group_rows_available_ -= num_values;
  chunk_read_offset_ = trans.GetLocation();

  return num_values;
}

void ColumnReader::Skip(idx_t num_values) {
  dummy_define_.Zero();
  dummy_repeat_.Zero();

  // Note: the offsets are still bit-unpacked even though the values are discarded.
  Vector dummy_result(logical_type_, nullptr);
  auto values_read = Read(num_values, none_filter_, reinterpret_cast<uint8_t *>(dummy_define_.ptr_),
                          reinterpret_cast<uint8_t *>(dummy_repeat_.ptr_), dummy_result);
  if (values_read != num_values) {
    throw Exception("Row count mismatch when skipping rows");
  }
}

}  // namespace bumblebee
