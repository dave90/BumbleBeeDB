//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_file.h
//
// Identification: src/include/storage/parquet/parquet_file.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "common/config.h"
#include "common/exception.h"

namespace bumblebee {

/**
 * @brief Positional read handle over a parquet file on the local filesystem.
 *
 * The parquet layer does its own file IO (footers and pages are read at absolute offsets),
 * independent of the page-oriented storage DiskManager.
 */
class ParquetFileHandle {
 public:
  /**
   * @brief Open `path` for reading.
   *
   * @param path Filesystem path of the parquet file.
   * @return The open handle.
   */
  static auto OpenForRead(const std::string &path) -> std::unique_ptr<ParquetFileHandle>;

  ParquetFileHandle(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ParquetFileHandle(const ParquetFileHandle &) = delete;
  auto operator=(const ParquetFileHandle &) -> ParquetFileHandle & = delete;
  ~ParquetFileHandle();

  /**
   * @brief Read exactly `len` bytes at absolute offset `location`.
   *
   * @param buffer Destination buffer.
   * @param len Number of bytes to read.
   * @param location Absolute file offset to read from.
   */
  void Read(void *buffer, idx_t len, idx_t location) const;

  /** @return The current size of the file in bytes. */
  auto FileSize() const -> idx_t;

  /** @return The file's last-modification time (used as a cheap footer-cache change detector). */
  auto LastModifiedTime() const -> time_t;

  /** The path this handle was opened with. */
  std::string path_;

 private:
  int fd_;
};

/** @brief Abstract byte sink the parquet writer serializes into. */
class Serializer {
 public:
  virtual ~Serializer() = default;

  /** @brief Append `write_size` raw bytes. */
  virtual void WriteData(const_data_ptr_t buffer, idx_t write_size) = 0;

  /** @brief Append one fixed-width value. */
  template <class T>
  void Write(T value) {
    WriteData(reinterpret_cast<const_data_ptr_t>(&value), sizeof(T));
  }
};

/** An owned, growable byte blob produced by a BufferedSerializer. */
struct BinaryData {
  std::unique_ptr<data_t[]> data_;
  idx_t size_{0};
};

/**
 * @brief A Serializer writing into a growable in-memory buffer (a parquet page is staged here
 * before compression, because the page header needs the final uncompressed size).
 */
class BufferedSerializer : public Serializer {
 public:
  static constexpr idx_t INITIAL_SIZE = 1024;

  explicit BufferedSerializer(idx_t maximum_size = INITIAL_SIZE) : maximum_size_(maximum_size) {
    blob_.data_ = std::unique_ptr<data_t[]>(new data_t[maximum_size]);
    blob_.size_ = 0;
    data_ = blob_.data_.get();
  }

  void WriteData(const_data_ptr_t buffer, idx_t write_size) override;

  /** The staged bytes written so far. */
  BinaryData blob_;

 private:
  idx_t maximum_size_;
  data_ptr_t data_;
};

/**
 * @brief A Serializer appending to a file through a small write-behind buffer, tracking the
 * total bytes written (parquet metadata records absolute offsets).
 */
class BufferedFileWriter : public Serializer {
 public:
  /** @brief Create/truncate `path` for writing. */
  explicit BufferedFileWriter(const std::string &path);
  BufferedFileWriter(const BufferedFileWriter &) = delete;
  auto operator=(const BufferedFileWriter &) -> BufferedFileWriter & = delete;
  ~BufferedFileWriter() override;

  void WriteData(const_data_ptr_t buffer, idx_t write_size) override;

  /** @brief Flush the buffer and fsync the file. */
  void Sync();

  /** @return Total bytes written since the file was opened. */
  auto GetTotalWritten() const -> idx_t { return total_written_; }

 private:
  static constexpr idx_t FILE_BUFFER_SIZE = 4096;

  void Flush();

  std::string path_;
  int fd_;
  std::unique_ptr<data_t[]> buffer_;
  idx_t offset_{0};
  idx_t total_written_{0};
};

}  // namespace bumblebee
