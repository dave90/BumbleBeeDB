//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// column_reader.h
//
// Identification: src/include/storage/parquet/column_reader.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <bitset>
#include <memory>
#include <vector>

#include "common/exception.h"
#include "common/macros.h"
#include "parquet/parquet_types.h"
#include "storage/parquet/resizeable_buffer.h"
#include "storage/parquet/rle_bp_decoder.h"
#include "storage/parquet/thrift_tools.h"
#include "type/logical_type.h"
#include "type/vector/vector.h"

namespace bumblebee {

class ParquetReader;

using thrift::protocol::TProtocol;

using format::ColumnChunk;
using format::FieldRepetitionType;
using format::PageHeader;
using format::SchemaElement;

/** Per-row mask: bit set = the row must be decoded (all set when no filter is pushed down). */
using parquet_filter_t = std::bitset<STANDARD_VECTOR_SIZE>;

/**
 * @brief Decodes one parquet column chunk (pages of one column within a row group) into Vectors.
 * Subclasses specialize per type family; the base class drives page decompression, definition /
 * repetition levels and the dictionary machinery.
 */
class ColumnReader {
 public:
  ColumnReader(ParquetReader &reader, LogicalType type_l, const SchemaElement &schema, idx_t file_idx,
               idx_t max_define, idx_t max_repeat);
  virtual ~ColumnReader();

  /** @brief Position this reader at its column chunk within a row group. */
  virtual void InitializeRead(const std::vector<ColumnChunk> &columns, TProtocol &protocol) {
    BUMBLEBEE_ASSERT(file_idx_ < columns.size(), "parquet invariant violated");
    chunk_ = &columns[file_idx_];
    protocol_ = &protocol;
    BUMBLEBEE_ASSERT(chunk_ != nullptr, "parquet invariant violated");
    BUMBLEBEE_ASSERT(chunk_->__isset.meta_data, "parquet invariant violated");

    if (chunk_->__isset.file_path) {
      throw NotImplementedException("Only inlined data files are supported (no references)");
    }

    // Sometimes there is an extra offset for the dict. sometimes it's wrong.
    chunk_read_offset_ = chunk_->meta_data.data_page_offset;
    if (chunk_->meta_data.__isset.dictionary_page_offset && chunk_->meta_data.dictionary_page_offset >= 4) {
      // this assumes the data pages follow the dict pages directly.
      chunk_read_offset_ = chunk_->meta_data.dictionary_page_offset;
    }
    group_rows_available_ = chunk_->meta_data.num_values;
  }

  /**
   * @brief Decode `num_values` rows into `result`.
   *
   * @param num_values Rows to decode.
   * @param filter Per-row decode mask (skipped rows are not materialized).
   * @param define_out Scratch for definition levels (NULL detection).
   * @param repeat_out Scratch for repetition levels.
   * @param result Output vector.
   * @return Rows decoded.
   */
  virtual auto Read(uint64_t num_values, parquet_filter_t &filter, uint8_t *define_out, uint8_t *repeat_out,
                    Vector &result) -> idx_t;

  /** @brief Skip `num_values` rows without materializing them. */
  virtual void Skip(idx_t num_values);

  /** @return The column's logical type. */
  auto GetLogicalType() const -> const LogicalType & { return logical_type_; }

  /** @return The column's schema element in the file footer. */
  auto GetSchema() const -> const SchemaElement & { return schema_; }

  /** @return Rows remaining in the current row group. */
  virtual auto GroupRowsAvailable() -> idx_t { return group_rows_available_; }

  /** @brief Instantiate the right reader subclass for a leaf column. */
  static auto CreateReader(ParquetReader &reader, const LogicalType &type_l, const SchemaElement &schema,
                           idx_t file_idx, idx_t max_define, idx_t max_repeat) -> std::unique_ptr<ColumnReader>;

 protected:
  // Readers that use the default Read() implement these.
  virtual void Plain(std::shared_ptr<ByteBuffer> plain_data, uint8_t *defines, idx_t num_values,
                     parquet_filter_t &filter, idx_t result_offset, Vector &result) {
    throw NotImplementedException("Parquet reader error: Plain");
  }

  virtual void Dictionary(std::shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) {
    throw NotImplementedException("Parquet reader error: Dictionary");
  }

  virtual void Offsets(uint32_t *offsets, uint8_t *defines, idx_t num_values, parquet_filter_t &filter,
                       idx_t result_offset, Vector &result) {
    throw NotImplementedException("Parquet reader error: Offsets");
  }

  // These are nops for most types, but not for strings (zero-copy buffer references).
  virtual void DictReference(Vector &result) {}
  virtual void PlainReference(std::shared_ptr<ByteBuffer> /*data*/, Vector &result) {}

  auto HasDefines() const -> bool { return max_define_ > 0; }
  auto HasRepeats() const -> bool { return max_repeat_ > 0; }

  /**
   * @brief True when no value in defines[offset, offset+count) is NULL (i.e. every definition
   * level equals the column maximum). Lets decode loops take a branch-free batch path for the
   * common all-valid case.
   */
  auto AllDefined(const uint8_t *defines, idx_t offset, idx_t count) const -> bool {
    if (!HasDefines()) {
      return true;
    }
    for (idx_t i = 0; i < count; i++) {
      if (defines[i + offset] != max_define_) {
        return false;
      }
    }
    return true;
  }

  const SchemaElement &schema_;

  idx_t file_idx_;
  idx_t max_define_;
  idx_t max_repeat_;

  ParquetReader &reader_;
  LogicalType logical_type_;

 private:
  void PrepareRead(parquet_filter_t &filter);
  void PreparePage(idx_t compressed_page_size, idx_t uncompressed_page_size, bool poolable);
  void PrepareDataPage(PageHeader &page_hdr);

  // Acquire a decompressed-page buffer from the reader-local pool, reusing one no longer
  // referenced by any live output chunk (use_count == 1) instead of allocating a fresh one.
  // Data pages are referenced zero-copy by string columns, so a single block_ slot cannot be
  // reused while a chunk still holds the previous page; without a pool every page hits
  // malloc/free of a large buffer, so each page faults in fresh zero pages and the kernel
  // mmap path serializes scan threads. Recycling a small set of buffers avoids that churn.
  auto AcquireDecompressed(idx_t size) -> std::shared_ptr<ResizeableBuffer>;

  const format::ColumnChunk *chunk_{nullptr};

  TProtocol *protocol_{nullptr};
  idx_t page_rows_available_;
  idx_t group_rows_available_{0};
  idx_t chunk_read_offset_{0};

  std::shared_ptr<ResizeableBuffer> block_;
  // Reused per-page compressed read buffer (transient decompressor input).
  ResizeableBuffer compressed_buffer_;

  // Pool of decompressed data-page buffers recycled across pages (see AcquireDecompressed).
  // Dictionary-page buffers are not pooled (one per row group, moved into the dictionary),
  // so this only holds the small working set of per-page buffers in flight for this column.
  std::vector<std::shared_ptr<ResizeableBuffer>> decompressed_pool_;

  ResizeableBuffer offset_buffer_;

  std::unique_ptr<RleBpDecoder> dict_decoder_;
  std::unique_ptr<RleBpDecoder> defined_decoder_;
  std::unique_ptr<RleBpDecoder> repeated_decoder_;

  // Dummies for Skip().
  parquet_filter_t none_filter_;
  ResizeableBuffer dummy_define_;
  ResizeableBuffer dummy_repeat_;
};

}  // namespace bumblebee
