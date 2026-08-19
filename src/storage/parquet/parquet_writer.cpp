//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_writer.cpp
//
// Identification: src/storage/parquet/parquet_writer.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_writer.h"

#include <cstring>
#include <optional>

#include "common/limits.h"
#include "miniz/miniz_wrapper.hpp"
#include "storage/parquet/parquet_timestamp.h"
#include "snappy/snappy.h"
#include "thrift/protocol/TCompactProtocol.h"
#include "type/vector/operations/vector_operations.h"
#include "zstd/include/zstd.h"

namespace bumblebee {

using format::CompressionCodec;
using format::ConvertedType;
using format::Encoding;
using format::FieldRepetitionType;
using format::PageHeader;
using format::PageType;
using format::Type;
using ParquetRowGroup = format::RowGroup;

/** Thrift transport writing into a Serializer (page headers / footer land in the file writer). */
class SerializerTransport : public thrift::transport::TTransport {
 public:
  explicit SerializerTransport(Serializer &serializer) : serializer_(serializer) {}

  auto isOpen() const -> bool override { return true; }
  void open() override {}
  void close() override {}

  void write_virt(const uint8_t *buf, uint32_t len) override {
    serializer_.WriteData(reinterpret_cast<const_data_ptr_t>(buf), len);
  }

 private:
  Serializer &serializer_;
};

static auto BumbleBeeTypeToParquetType(const LogicalType &type) -> Type::type {
  switch (type.GetTypeId()) {
    case LogicalTypeId::BOOLEAN:
      return Type::BOOLEAN;
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
      return Type::INT32;
    case LogicalTypeId::BIGINT:
      return Type::INT64;
    case LogicalTypeId::FLOAT:
      return Type::FLOAT;
    case LogicalTypeId::DECIMAL:  // decimals are written as doubles for now
    case LogicalTypeId::DOUBLE:
      return Type::DOUBLE;
    case LogicalTypeId::STRING:
      return Type::BYTE_ARRAY;
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIMESTAMP:
      return Type::INT96;
    default:
      throw NotImplementedException("Parquet write type not supported: " + type.ToString());
  }
}

static auto BumbleBeeTypeToConvertedType(const LogicalType &type, ConvertedType::type &result) -> bool {
  switch (type.GetTypeId()) {
    case LogicalTypeId::STRING:
      result = ConvertedType::UTF8;
      return true;
    default:
      return false;
  }
}

static void VarintEncode(uint32_t val, Serializer &ser) {
  do {
    uint8_t byte = val & 127;
    val >>= 7;
    if (val != 0) {
      byte |= 128;
    }
    ser.Write<uint8_t>(byte);
  } while (val != 0);
}

static auto GetVarintSize(uint32_t val) -> uint8_t {
  uint8_t res = 0;
  do {
    val >>= 7;
    res++;
  } while (val != 0);
  return res;
}

template <class SRC, class TGT, bool HAS_NULL>
static void TemplatedWritePlainNullCheck(Vector &col, idx_t length, Serializer &ser) {
  auto *ptr = FlatVector::GetData<SRC>(col);
  if constexpr (!HAS_NULL) {
    for (idx_t r = 0; r < length; r++) {
      ser.Write<TGT>(static_cast<TGT>(ptr[r]));
    }
  } else {
    // NULL rows have no payload entry (see definition levels).
    for (idx_t r = 0; r < length; r++) {
      if (col.RowIsValid(r)) {
        ser.Write<TGT>(static_cast<TGT>(ptr[r]));
      }
    }
  }
}

template <class SRC, class TGT>
static void TemplatedWritePlain(Vector &col, idx_t length, Serializer &ser) {
  if (col.Validity().AllValid()) {
    TemplatedWritePlainNullCheck<SRC, TGT, false>(col, length, ser);
  } else {
    TemplatedWritePlainNullCheck<SRC, TGT, true>(col, length, ser);
  }
}

/**
 * @brief Min/max of one numeric column across the collection (nulls skipped), encoded as the
 * plain-encoded statistics payload (raw little-endian TGT bytes) parquet readers expect.
 */
template <class SRC, class TGT>
static auto ComputeNumericStats(ChunkCollection &buffer, idx_t col)
    -> std::optional<std::pair<std::string, std::string>> {
  bool seen = false;
  TGT min_v{};
  TGT max_v{};
  for (auto &chunk : buffer.Chunks()) {
    auto &vec = chunk->data_[col];
    auto *ptr = FlatVector::GetData<SRC>(vec);
    for (idx_t r = 0; r < chunk->GetSize(); r++) {
      if (!vec.RowIsValid(r)) {
        continue;
      }
      const auto v = static_cast<TGT>(ptr[r]);
      if (!seen || v < min_v) {
        min_v = v;
      }
      if (!seen || v > max_v) {
        max_v = v;
      }
      seen = true;
    }
  }
  if (!seen) {
    return std::nullopt;
  }
  std::string min_bytes(reinterpret_cast<const char *>(&min_v), sizeof(TGT));
  std::string max_bytes(reinterpret_cast<const char *>(&max_v), sizeof(TGT));
  return std::make_pair(std::move(min_bytes), std::move(max_bytes));
}

/** @brief Column-chunk statistics for the plain numeric types (zone-map fodder for the scan). */
static auto NumericStatsFor(const LogicalType &type, ChunkCollection &buffer, idx_t col)
    -> std::optional<std::pair<std::string, std::string>> {
  switch (type.GetTypeId()) {
    case LogicalTypeId::TINYINT:
      return ComputeNumericStats<int8_t, int32_t>(buffer, col);
    case LogicalTypeId::SMALLINT:
      return ComputeNumericStats<int16_t, int32_t>(buffer, col);
    case LogicalTypeId::INTEGER:
      return ComputeNumericStats<int32_t, int32_t>(buffer, col);
    case LogicalTypeId::BIGINT:
      return ComputeNumericStats<int64_t, int64_t>(buffer, col);
    case LogicalTypeId::FLOAT:
      return ComputeNumericStats<float, float>(buffer, col);
    case LogicalTypeId::DOUBLE:
      return ComputeNumericStats<double, double>(buffer, col);
    default:
      return std::nullopt;  // strings/decimals/timestamps carry no stats (yet)
  }
}

ParquetWriter::ParquetWriter(std::string file_name, std::vector<LogicalType> types, std::vector<std::string> names,
                             CompressionCodec::type codec)
    : file_name_(std::move(file_name)), sql_types_(std::move(types)), column_names_(std::move(names)), codec_(codec) {
  BUMBLEBEE_ASSERT(sql_types_.size() == column_names_.size(), "parquet invariant violated");

  writer_ = std::make_unique<BufferedFileWriter>(file_name_);
  // Parquet files start with the string "PAR1".
  writer_->WriteData(reinterpret_cast<const_data_ptr_t>("PAR1"), 4);
  thrift::protocol::TCompactProtocolFactoryT<SerializerTransport> tproto_factory;
  protocol_ = tproto_factory.getProtocol(std::make_shared<SerializerTransport>(*writer_));

  file_meta_data_.num_rows = 0;
  file_meta_data_.version = 1;

  file_meta_data_.__isset.created_by = true;
  file_meta_data_.created_by = "BumbleBee";

  file_meta_data_.schema.resize(sql_types_.size() + 1);

  // Populate the root schema object.
  file_meta_data_.schema[0].name = "bumblebee_schema";
  file_meta_data_.schema[0].num_children = sql_types_.size();
  file_meta_data_.schema[0].__isset.num_children = true;

  for (idx_t i = 0; i < sql_types_.size(); i++) {
    auto &schema_element = file_meta_data_.schema[i + 1];

    schema_element.type = BumbleBeeTypeToParquetType(sql_types_[i]);
    schema_element.repetition_type = FieldRepetitionType::OPTIONAL;
    schema_element.num_children = 0;
    schema_element.__isset.num_children = true;
    schema_element.__isset.type = true;
    schema_element.__isset.repetition_type = true;
    schema_element.name = column_names_[i];
    schema_element.__isset.converted_type = BumbleBeeTypeToConvertedType(sql_types_[i], schema_element.converted_type);
  }
}

/** @brief Write column `col`'s definition levels as one RLE bit-packed-literals run: bit=1 valid,
 * bit=0 NULL (LSB-first). STANDARD_VECTOR_SIZE is a multiple of 8, so per-chunk byte-aligned
 * packing stays contiguous for the reader (only the last chunk pads). */
static void WriteDefinitionLevels(ChunkCollection &buffer, idx_t col, Serializer &temp_writer) {
  // First figure out how many bytes we need (1 byte per 8 rows, rounded up); the count is a
  // varint header with the low bit set to indicate bit-packed literals.
  auto define_byte_count = (buffer.GetCount() + 7) / 8;
  uint32_t define_header = (define_byte_count << 1) | 1;
  uint32_t define_size = GetVarintSize(define_header) + define_byte_count;

  temp_writer.Write<uint32_t>(define_size);
  VarintEncode(define_header, temp_writer);

  for (auto &chunk : buffer.Chunks()) {
    BUMBLEBEE_ASSERT(chunk->GetSize() <= STANDARD_VECTOR_SIZE, "parquet invariant violated");
    auto &define_col = chunk->data_[col];
    auto chunk_define_byte_count = (chunk->GetSize() + 7) / 8;
    uint8_t define_buf[(STANDARD_VECTOR_SIZE + 7) / 8];
    if (define_col.Validity().AllValid()) {
      std::memset(define_buf, 0xFF, chunk_define_byte_count);
    } else {
      std::memset(define_buf, 0, chunk_define_byte_count);
      for (idx_t r = 0; r < chunk->GetSize(); r++) {
        if (define_col.RowIsValid(r)) {
          define_buf[r / 8] |= static_cast<uint8_t>(1u << (r % 8));
        }
      }
    }
    temp_writer.WriteData(reinterpret_cast<const_data_ptr_t>(define_buf), chunk_define_byte_count);
  }
}

/** @brief Write column `col`'s non-NULL values as PLAIN-encoded payload, chunk by chunk. */
static void WritePlainColumnPayload(const LogicalType &type, ChunkCollection &buffer, idx_t col,
                                    Serializer &temp_writer) {
  for (auto &chunk : buffer.Chunks()) {
    auto &input = *chunk;
    auto &input_column = input.data_[col];

    switch (type.GetTypeId()) {
      case LogicalTypeId::BOOLEAN: {
        auto *ptr = FlatVector::GetData<uint8_t>(input_column);
        uint8_t byte = 0;
        uint8_t byte_pos = 0;
        for (idx_t r = 0; r < input.GetSize(); r++) {
          if (!input_column.RowIsValid(r)) {
            continue;  // NULL: no payload entry
          }
          byte |= (ptr[r] & 1) << byte_pos;
          byte_pos++;

          if (byte_pos == 8) {
            temp_writer.Write<uint8_t>(byte);
            byte = 0;
            byte_pos = 0;
          }
        }
        if (byte_pos > 0) {
          temp_writer.Write<uint8_t>(byte);
        }
        break;
      }
      case LogicalTypeId::TINYINT:
        TemplatedWritePlain<int8_t, int32_t>(input_column, input.GetSize(), temp_writer);
        break;
      case LogicalTypeId::SMALLINT:
        TemplatedWritePlain<int16_t, int32_t>(input_column, input.GetSize(), temp_writer);
        break;
      case LogicalTypeId::INTEGER:
        TemplatedWritePlain<int32_t, int32_t>(input_column, input.GetSize(), temp_writer);
        break;
      case LogicalTypeId::BIGINT:
        TemplatedWritePlain<int64_t, int64_t>(input_column, input.GetSize(), temp_writer);
        break;
      case LogicalTypeId::FLOAT:
        TemplatedWritePlain<float, float>(input_column, input.GetSize(), temp_writer);
        break;
      case LogicalTypeId::DECIMAL: {
        // Written as DOUBLE until FIXED_LEN_BYTE_ARRAY decimals are implemented.
        Vector double_vec{LogicalType(LogicalTypeId::DOUBLE)};
        VectorOperations::Cast(input_column, double_vec, input.GetSize());
        TemplatedWritePlain<double, double>(double_vec, input.GetSize(), temp_writer);
        break;
      }
      case LogicalTypeId::DOUBLE:
        TemplatedWritePlain<double, double>(input_column, input.GetSize(), temp_writer);
        break;
      case LogicalTypeId::DATE: {
        auto *ptr = FlatVector::GetData<date_t>(input_column);
        for (idx_t r = 0; r < input.GetSize(); r++) {
          if (!input_column.RowIsValid(r)) {
            continue;  // NULL: no payload entry
          }
          auto ts = Timestamp::FromDatetime(ptr[r]);
          temp_writer.Write<Int96>(TimestampToImpalaTimestamp(ts));
        }
        break;
      }
      case LogicalTypeId::TIMESTAMP: {
        auto *ptr = FlatVector::GetData<timestamp_t>(input_column);
        for (idx_t r = 0; r < input.GetSize(); r++) {
          if (!input_column.RowIsValid(r)) {
            continue;  // NULL: no payload entry
          }
          temp_writer.Write<Int96>(TimestampToImpalaTimestamp(ptr[r]));
        }
        break;
      }
      case LogicalTypeId::STRING: {
        auto *ptr = FlatVector::GetData<string_t>(input_column);
        for (idx_t r = 0; r < input.GetSize(); r++) {
          if (!input_column.RowIsValid(r)) {
            continue;  // NULL: no payload entry
          }
          temp_writer.Write<uint32_t>(ptr[r].Size());
          temp_writer.WriteData(reinterpret_cast<const_data_ptr_t>(ptr[r].GetDataUnsafe()), ptr[r].Size());
        }
        break;
      }
      default:
        throw NotImplementedException("Parquet write type not supported: " + type.ToString());
    }
  }
}

/** @brief Compress the assembled page with `codec`. `holder` owns the compressed bytes when the
 * codec allocates (UNCOMPRESSED aliases the page buffer instead — keep both alive until the
 * returned span is written out). @return (data, size) of the bytes to write. */
static auto CompressPage(CompressionCodec::type codec, BufferedSerializer &temp_writer,
                         std::unique_ptr<data_t[]> &holder) -> std::pair<data_ptr_t, size_t> {
  size_t compressed_size;
  data_ptr_t compressed_data;
  switch (codec) {
    case CompressionCodec::UNCOMPRESSED:
      compressed_size = temp_writer.blob_.size_;
      compressed_data = temp_writer.blob_.data_.get();
      break;
    case CompressionCodec::SNAPPY: {
      compressed_size = snappy::MaxCompressedLength(temp_writer.blob_.size_);
      holder = std::make_unique_for_overwrite<data_t[]>(compressed_size);
      snappy::RawCompress(reinterpret_cast<const char *>(temp_writer.blob_.data_.get()), temp_writer.blob_.size_,
                          reinterpret_cast<char *>(holder.get()), &compressed_size);
      compressed_data = holder.get();
      break;
    }
    case CompressionCodec::GZIP: {
      MiniZStream s;
      compressed_size = s.MaxCompressedLength(temp_writer.blob_.size_);
      holder = std::make_unique_for_overwrite<data_t[]>(compressed_size);
      s.Compress(reinterpret_cast<const char *>(temp_writer.blob_.data_.get()), temp_writer.blob_.size_,
                 reinterpret_cast<char *>(holder.get()), &compressed_size);
      compressed_data = holder.get();
      break;
    }
    case CompressionCodec::ZSTD: {
      compressed_size = ZSTD_compressBound(temp_writer.blob_.size_);
      holder = std::make_unique_for_overwrite<data_t[]>(compressed_size);
      compressed_size = ZSTD_compress(holder.get(), compressed_size, temp_writer.blob_.data_.get(),
                                      temp_writer.blob_.size_, ZSTD_CLEVEL_DEFAULT);
      compressed_data = holder.get();
      break;
    }
    default:
      throw NotImplementedException("Unsupported codec for Parquet Writer");
  }
  return {compressed_data, compressed_size};
}

void ParquetWriter::Flush(ChunkCollection &buffer) {
  if (buffer.GetCount() == 0) {
    return;
  }
  std::lock_guard<std::mutex> glock(lock_);

  // Set up a new row group for this chunk collection.
  ParquetRowGroup row_group;
  row_group.num_rows = 0;
  row_group.file_offset = writer_->GetTotalWritten();
  row_group.__isset.file_offset = true;
  row_group.columns.resize(buffer.ColumnCount());

  // Iterate over each of the columns of the chunk collection and write them.
  for (idx_t i = 0; i < buffer.ColumnCount(); i++) {
    // We start off by writing everything into a temporary buffer. This is necessary to (1)
    // know the total written size, and (2) to compress it afterwards.
    BufferedSerializer temp_writer;

    // Set up some metadata.
    PageHeader hdr;
    hdr.compressed_page_size = 0;
    hdr.uncompressed_page_size = 0;
    hdr.type = PageType::DATA_PAGE;
    hdr.__isset.data_page_header = true;

    hdr.data_page_header.num_values = buffer.GetCount();
    hdr.data_page_header.encoding = Encoding::PLAIN;
    hdr.data_page_header.definition_level_encoding = Encoding::RLE;
    hdr.data_page_header.repetition_level_encoding = Encoding::BIT_PACKED;

    // Record the current offset of the writer into the file: the starting position of the
    // current page.
    auto start_offset = writer_->GetTotalWritten();

    WriteDefinitionLevels(buffer, i, temp_writer);
    WritePlainColumnPayload(sql_types_[i], buffer, i, temp_writer);

    // Now that we have finished writing the data we know the uncompressed size.
    hdr.uncompressed_page_size = temp_writer.blob_.size_;

    std::unique_ptr<data_t[]> compressed_buf;
    auto [compressed_data, compressed_size] = CompressPage(codec_, temp_writer, compressed_buf);

    hdr.compressed_page_size = compressed_size;
    // Now finally write the data to the actual file.
    hdr.write(protocol_.get());
    writer_->WriteData(compressed_data, compressed_size);

    auto &column_chunk = row_group.columns[i];
    column_chunk.__isset.meta_data = true;
    column_chunk.meta_data.data_page_offset = start_offset;
    column_chunk.meta_data.total_compressed_size = writer_->GetTotalWritten() - start_offset;
    column_chunk.meta_data.codec = codec_;
    column_chunk.meta_data.path_in_schema.push_back(file_meta_data_.schema[i + 1].name);
    column_chunk.meta_data.num_values = buffer.GetCount();
    column_chunk.meta_data.type = file_meta_data_.schema[i + 1].type;
    // Min/max statistics let scans prune whole row groups against simple predicates.
    if (auto stats = NumericStatsFor(sql_types_[i], buffer, i); stats.has_value()) {
      column_chunk.meta_data.statistics.__set_min_value(stats->first);
      column_chunk.meta_data.statistics.__set_max_value(stats->second);
      column_chunk.meta_data.__isset.statistics = true;
    }
  }
  row_group.num_rows += buffer.GetCount();

  // Append the row group to the file meta data.
  file_meta_data_.row_groups.push_back(row_group);
  file_meta_data_.num_rows += buffer.GetCount();
}

void ParquetWriter::Finalize() {
  auto start_offset = writer_->GetTotalWritten();
  file_meta_data_.write(protocol_.get());

  writer_->Write<uint32_t>(writer_->GetTotalWritten() - start_offset);

  // Parquet files also end with the string "PAR1".
  writer_->WriteData(reinterpret_cast<const_data_ptr_t>("PAR1"), 4);

  // Flush to disk.
  writer_->Sync();
  writer_.reset();
}

}  // namespace bumblebee
