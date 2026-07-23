//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_reader.cpp
//
// Identification: src/storage/parquet/parquet_reader.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_reader.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "storage/parquet/struct_column_reader.h"
#include "thrift/protocol/TCompactProtocol.h"

namespace bumblebee {

auto GlobalParquetAllocator() -> Allocator & {
  static Allocator allocator;
  return allocator;
}

using format::ConvertedType;
using format::FieldRepetitionType;
using format::Type;

static auto CreateThriftProtocol(Allocator &allocator, ParquetFileHandle &file_handle)
    -> std::unique_ptr<thrift::protocol::TProtocol> {
  auto transport = std::make_shared<ThriftFileTransport>(allocator, file_handle);
  return std::make_unique<thrift::protocol::TCompactProtocolT<ThriftFileTransport>>(std::move(transport));
}

// Process-wide cache of parsed parquet footers, keyed by path + size + mtime. The footer of a
// wide many-row-group file is megabytes of thrift and is otherwise parsed several times per
// query (bind, max-thread estimation, scan); parsing once and sharing the immutable
// FileMetaData removes that fixed per-query overhead, which dominates the wall time of
// small/selective scans over large files.
static std::mutex g_md_cache_mutex;
static std::unordered_map<std::string, std::shared_ptr<ParquetFileMetadataCache>> g_md_cache;

static auto LoadMetadata(Allocator &allocator, ParquetFileHandle &file_handle)
    -> std::shared_ptr<ParquetFileMetadataCache> {
  auto current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  auto proto = CreateThriftProtocol(allocator, file_handle);
  auto &transport = reinterpret_cast<ThriftFileTransport &>(*proto->getTransport());
  auto file_size = transport.GetSize();

  // Cache lookup: identity is the path plus current size and mtime (a cheap change detector
  // for read-mostly analytics files).
  time_t mtime = file_handle.LastModifiedTime();
  std::string cache_key =
      file_handle.path_ + ":" + std::to_string(file_size) + ":" + std::to_string(static_cast<long long>(mtime));
  {
    std::lock_guard<std::mutex> lock(g_md_cache_mutex);
    auto it = g_md_cache.find(cache_key);
    if (it != g_md_cache.end()) {
      return it->second;
    }
  }
  if (file_size < 12) {
    throw Exception(fmt::format("File '{}' too small to be a Parquet file", file_handle.path_));
  }

  ResizeableBuffer buf;
  buf.Resize(allocator, 8);
  buf.Zero();

  transport.SetLocation(file_size - 8);
  transport.read(reinterpret_cast<uint8_t *>(buf.ptr_), 8);

  if (strncmp(buf.ptr_ + 4, "PAR1", 4) != 0) {
    throw Exception(fmt::format("No magic bytes found at end of file '{}'", file_handle.path_));
  }
  // Read the four-byte footer length from just before the end magic bytes.
  auto footer_len = *reinterpret_cast<uint32_t *>(buf.ptr_);
  if (footer_len == 0 || file_size < 12 + footer_len) {
    throw Exception(fmt::format("Footer length error in file '{}'", file_handle.path_));
  }
  auto metadata_pos = file_size - (footer_len + 8);
  transport.SetLocation(metadata_pos);
  transport.Prefetch(metadata_pos, footer_len);

  auto metadata = std::make_unique<format::FileMetaData>();
  metadata->read(proto.get());
  auto cache = std::make_shared<ParquetFileMetadataCache>(std::move(metadata), current_time);
  {
    std::lock_guard<std::mutex> lock(g_md_cache_mutex);
    // Bound the cache so a long-lived process scanning many distinct files cannot grow it
    // without limit; footers are immutable so a coarse cap is fine.
    if (g_md_cache.size() > 256) {
      g_md_cache.clear();
    }
    g_md_cache[cache_key] = cache;
  }
  return cache;
}

auto ParquetReader::DeriveLogicalType(const format::SchemaElement &s_ele) -> LogicalType {
  // Leaf node.
  BUMBLEBEE_ASSERT(s_ele.__isset.type && s_ele.num_children == 0, "parquet invariant violated");
  switch (s_ele.type) {
    case Type::BOOLEAN:
      return LogicalType(LogicalTypeId::BOOLEAN);
    case Type::INT32:
      if (s_ele.__isset.converted_type) {
        switch (s_ele.converted_type) {
          case ConvertedType::DATE:
            return LogicalType(LogicalTypeId::DATE);
          case ConvertedType::DECIMAL:
            return LogicalType::Decimal(9, s_ele.scale);
          case ConvertedType::UINT_8:
            return LogicalType(LogicalTypeId::UTINYINT);
          case ConvertedType::UINT_16:
            return LogicalType(LogicalTypeId::USMALLINT);
          default:
            return LogicalType(LogicalTypeId::INTEGER);
        }
      }
      return LogicalType(LogicalTypeId::INTEGER);
    case Type::INT64:
      if (s_ele.__isset.converted_type) {
        switch (s_ele.converted_type) {
          case ConvertedType::DECIMAL:
            return LogicalType::Decimal(18, s_ele.scale);
          case ConvertedType::TIMESTAMP_MICROS:
          case ConvertedType::TIMESTAMP_MILLIS:
            return LogicalType(LogicalTypeId::TIMESTAMP);
          case ConvertedType::UINT_32:
            return LogicalType(LogicalTypeId::UINTEGER);
          case ConvertedType::UINT_64:
            return LogicalType(LogicalTypeId::UBIGINT);
          default:
            return LogicalType(LogicalTypeId::BIGINT);
        }
      }
      return LogicalType(LogicalTypeId::BIGINT);
    case Type::INT96:
      // Impala-style timestamp.
      return LogicalType(LogicalTypeId::TIMESTAMP);
    case Type::FLOAT:
      return LogicalType(LogicalTypeId::FLOAT);
    case Type::DOUBLE:
      return LogicalType(LogicalTypeId::DOUBLE);
    case Type::BYTE_ARRAY:
    case Type::FIXED_LEN_BYTE_ARRAY:
      if (s_ele.type == Type::FIXED_LEN_BYTE_ARRAY && !s_ele.__isset.type_length) {
        throw Exception(fmt::format("Failed to read Parquet file \"{}\": ARRAY parquet type is not supported",
                                    file_name_));
      }
      if (s_ele.__isset.converted_type) {
        switch (s_ele.converted_type) {
          case ConvertedType::DECIMAL:
            if (s_ele.type == Type::FIXED_LEN_BYTE_ARRAY && s_ele.__isset.scale && s_ele.__isset.type_length) {
              return LogicalType::Decimal(s_ele.precision, s_ele.scale);
            }
            throw Exception(fmt::format(
                "Failed to read Parquet file \"{}\": DECIMAL parquet type without scale and precision", file_name_));
          case ConvertedType::UTF8:
          default:
            return LogicalType(LogicalTypeId::STRING);
        }
      }
      return LogicalType(LogicalTypeId::STRING);
    default:
      return LogicalType(LogicalTypeId::UNKNOWN);
  }
}

auto ParquetReader::CreateReaderRecursive(const format::FileMetaData *file_meta_data, idx_t depth, idx_t max_define,
                                          idx_t max_repeat, idx_t &next_schema_idx, idx_t &next_file_idx)
    -> std::unique_ptr<ColumnReader> {
  BUMBLEBEE_ASSERT(file_meta_data != nullptr, "parquet invariant violated");
  BUMBLEBEE_ASSERT(next_schema_idx < file_meta_data->schema.size(), "parquet invariant violated");
  auto &s_ele = file_meta_data->schema[next_schema_idx];
  auto this_idx = next_schema_idx;

  if (s_ele.__isset.repetition_type) {
    if (s_ele.repetition_type != FieldRepetitionType::REQUIRED) {
      max_define++;
    }
    if (s_ele.repetition_type == FieldRepetitionType::REPEATED) {
      max_repeat++;
    }
  }

  if (!s_ele.__isset.type) {  // inner node
    if (depth > 0) {
      // Only the root may be a struct, not inner nodes.
      throw NotImplementedException(
          fmt::format("Failed to read Parquet file \"{}\": nested STRUCT parquet type not supported", file_name_));
    }
    if (s_ele.num_children == 0) {
      throw Exception("Parquet schema node has no children but should");
    }
    child_list_t<LogicalType> child_types;
    std::vector<std::unique_ptr<ColumnReader>> child_readers;

    idx_t c_idx = 0;
    while (c_idx < static_cast<idx_t>(s_ele.num_children)) {
      next_schema_idx++;

      auto &child_ele = file_meta_data->schema[next_schema_idx];

      auto child_reader =
          CreateReaderRecursive(file_meta_data, depth + 1, max_define, max_repeat, next_schema_idx, next_file_idx);
      child_types.emplace_back(child_ele.name, child_reader->GetLogicalType());
      child_readers.push_back(std::move(child_reader));

      c_idx++;
    }
    BUMBLEBEE_ASSERT(!child_types.empty(), "parquet invariant violated");
    if (s_ele.repetition_type == FieldRepetitionType::REPEATED) {
      throw NotImplementedException(
          fmt::format("Failed to read Parquet file \"{}\": LIST parquet type not supported", file_name_));
    }
    // The root is always modeled as a struct-of-columns.
    return std::make_unique<StructColumnReader>(*this, LogicalType(LogicalTypeId::STRUCT), s_ele, this_idx,
                                                max_define, max_repeat, std::move(child_readers),
                                                std::move(child_types));
  }
  // Leaf node.
  return ColumnReader::CreateReader(*this, DeriveLogicalType(s_ele), s_ele, next_file_idx++, max_define, max_repeat);
}

auto ParquetReader::CreateReader(const format::FileMetaData *file_meta_data) -> std::unique_ptr<ColumnReader> {
  idx_t next_schema_idx = 0;
  idx_t next_file_idx = 0;

  auto ret = CreateReaderRecursive(file_meta_data, 0, 0, 0, next_schema_idx, next_file_idx);
  BUMBLEBEE_ASSERT(next_schema_idx == file_meta_data->schema.size() - 1, "parquet invariant violated");
  BUMBLEBEE_ASSERT(file_meta_data->row_groups.empty() ||
                   next_file_idx == file_meta_data->row_groups[0].columns.size(), "parquet invariant violated");
  return ret;
}

void ParquetReader::InitializeSchema() {
  auto *file_meta_data = GetFileMetadata();

  if (file_meta_data->__isset.encryption_algorithm) {
    throw NotImplementedException(
        fmt::format("Failed to read Parquet file \"{}\": encrypted files are not supported", file_name_));
  }
  if (file_meta_data->schema.size() < 2) {
    throw Exception(
        fmt::format("Failed to read Parquet file \"{}\": need at least one non-root column", file_name_));
  }

  auto root_reader = CreateReader(file_meta_data);

  // The root is a struct containing all the column types.
  BUMBLEBEE_ASSERT(root_reader->GetLogicalType().GetTypeId() == LogicalTypeId::STRUCT, "parquet invariant violated");
  auto &child_types = static_cast<StructColumnReader &>(*root_reader).child_types_;

  for (auto &type_pair : child_types) {
    names_.push_back(type_pair.first);
    return_types_.push_back(type_pair.second);
  }
  BUMBLEBEE_ASSERT(!names_.empty(), "parquet invariant violated");
  BUMBLEBEE_ASSERT(!return_types_.empty(), "parquet invariant violated");
}

ParquetReader::ParquetReader(Allocator &allocator, std::string file_name)
    : allocator_(allocator), file_name_(std::move(file_name)) {
  file_handle_ = ParquetFileHandle::OpenForRead(file_name_);
  metadata_ = LoadMetadata(allocator_, *file_handle_);
  InitializeSchema();
}

ParquetReader::~ParquetReader() = default;

auto ParquetReader::GetFileMetadata() -> const format::FileMetaData * {
  BUMBLEBEE_ASSERT(metadata_, "parquet invariant violated");
  BUMBLEBEE_ASSERT(metadata_->metadata_, "parquet invariant violated");
  return metadata_->metadata_.get();
}

auto ParquetReader::GetGroup(ParquetReaderScanState &state) -> const format::RowGroup & {
  auto *file_meta_data = GetFileMetadata();
  BUMBLEBEE_ASSERT(state.current_group_ >= 0 &&
                   static_cast<idx_t>(state.current_group_) < state.group_idx_list_.size(), "parquet invariant violated");
  BUMBLEBEE_ASSERT(state.group_idx_list_[state.current_group_] < file_meta_data->row_groups.size(), "parquet invariant violated");
  return file_meta_data->row_groups[state.group_idx_list_[state.current_group_]];
}

void ParquetReader::PrepareRowGroupBuffer(ParquetReaderScanState &state, idx_t out_col_idx) {
  auto &group = GetGroup(state);
  state.root_reader_->InitializeRead(group.columns, *state.thrift_file_proto_);
}

auto ParquetReader::NumRows() -> idx_t { return GetFileMetadata()->num_rows; }

auto ParquetReader::NumRowGroups() -> idx_t { return GetFileMetadata()->row_groups.size(); }

void ParquetReader::InitializeScan(ParquetReaderScanState &state, std::vector<idx_t> column_ids,
                                   std::vector<idx_t> groups_to_read) {
  state.current_group_ = -1;
  state.finished_ = false;
  state.column_ids_ = std::move(column_ids);
  state.group_offset_ = 0;
  state.group_idx_list_ = std::move(groups_to_read);
  state.file_handle_ = ParquetFileHandle::OpenForRead(file_handle_->path_);
  state.thrift_file_proto_ = CreateThriftProtocol(allocator_, *state.file_handle_);
  state.root_reader_ = CreateReader(GetFileMetadata());

  state.define_buf_.Resize(allocator_, STANDARD_VECTOR_SIZE);
  state.repeat_buf_.Resize(allocator_, STANDARD_VECTOR_SIZE);
}

void ParquetReader::Scan(ParquetReaderScanState &state, DataChunk &result) {
  while (ScanInternal(state, result)) {
    if (result.GetSize() > 0) {
      break;
    }
    result.Reset();
  }
}

auto ParquetReader::ScanInternal(ParquetReaderScanState &state, DataChunk &result) -> bool {
  if (state.finished_) {
    return false;
  }

  // See if we have to switch to the next row group in the parquet file.
  if (state.current_group_ < 0 || static_cast<int64_t>(state.group_offset_) >= GetGroup(state).num_rows) {
    state.current_group_++;
    state.group_offset_ = 0;

    if (static_cast<idx_t>(state.current_group_) == state.group_idx_list_.size()) {
      state.finished_ = true;
      return false;
    }

    // Prepare the scan for each column.
    for (idx_t out_col_idx = 0; out_col_idx < result.ColumnCount(); out_col_idx++) {
      // Output columns with no file column behind them (e.g. synthetic row ids) are skipped.
      if (state.column_ids_[out_col_idx] == COLUMN_IDENTIFIER_ROW_ID) {
        continue;
      }
      PrepareRowGroupBuffer(state, out_col_idx);
      // InitializeRead positions every child reader at once; once is enough.
      break;
    }
    return true;
  }

  auto this_output_chunk_rows = MinValue<idx_t>(STANDARD_VECTOR_SIZE, GetGroup(state).num_rows - state.group_offset_);
  result.SetCardinality(this_output_chunk_rows);

  if (this_output_chunk_rows == 0) {
    state.finished_ = true;
    return false;  // end of last group, we are done
  }

  // No filter pushdown yet: decode every row of every requested column.
  parquet_filter_t filter_mask;
  filter_mask.set();

  state.define_buf_.Zero();
  state.repeat_buf_.Zero();

  auto *define_ptr = reinterpret_cast<uint8_t *>(state.define_buf_.ptr_);
  auto *repeat_ptr = reinterpret_cast<uint8_t *>(state.repeat_buf_.ptr_);

  auto *root_reader = static_cast<StructColumnReader *>(state.root_reader_.get());

  for (idx_t out_col_idx = 0; out_col_idx < result.ColumnCount(); out_col_idx++) {
    auto file_col_idx = state.column_ids_[out_col_idx];
    if (file_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
      continue;
    }
    root_reader->GetChildReader(file_col_idx)
        ->Read(result.GetSize(), filter_mask, define_ptr, repeat_ptr, result.data_[out_col_idx]);
  }

  state.group_offset_ += this_output_chunk_rows;
  return true;
}

}  // namespace bumblebee
