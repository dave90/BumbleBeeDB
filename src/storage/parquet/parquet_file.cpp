//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_file.cpp
//
// Identification: src/storage/parquet/parquet_file.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "fmt/format.h"

namespace bumblebee {

auto ParquetFileHandle::OpenForRead(const std::string &path) -> std::unique_ptr<ParquetFileHandle> {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    throw Exception(fmt::format("Cannot open file \"{}\": {}", path, strerror(errno)));
  }
  return std::make_unique<ParquetFileHandle>(path, fd);
}

ParquetFileHandle::~ParquetFileHandle() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

void ParquetFileHandle::Read(void *buffer, idx_t len, idx_t location) const {
  auto *dst = static_cast<char *>(buffer);
  idx_t done = 0;
  while (done < len) {
    ssize_t n = pread(fd_, dst + done, len - done, static_cast<off_t>(location + done));
    if (n < 0) {
      throw Exception(fmt::format("Read error in file \"{}\": {}", path_, strerror(errno)));
    }
    if (n == 0) {
      throw Exception(fmt::format("Unexpected end of file \"{}\" (truncated parquet file?)", path_));
    }
    done += static_cast<idx_t>(n);
  }
}

auto ParquetFileHandle::FileSize() const -> idx_t {
  struct stat st{};
  if (fstat(fd_, &st) != 0) {
    throw Exception(fmt::format("Cannot stat file \"{}\": {}", path_, strerror(errno)));
  }
  return static_cast<idx_t>(st.st_size);
}

auto ParquetFileHandle::LastModifiedTime() const -> time_t {
  struct stat st{};
  if (fstat(fd_, &st) != 0) {
    throw Exception(fmt::format("Cannot stat file \"{}\": {}", path_, strerror(errno)));
  }
  return st.st_mtime;
}

void BufferedSerializer::WriteData(const_data_ptr_t buffer, idx_t write_size) {
  if (blob_.size_ + write_size >= maximum_size_) {
    do {
      maximum_size_ *= 2;
    } while (blob_.size_ + write_size > maximum_size_);
    auto new_data = std::make_unique_for_overwrite<data_t[]>(maximum_size_);
    std::memcpy(new_data.get(), data_, blob_.size_);
    data_ = new_data.get();
    blob_.data_ = std::move(new_data);
  }
  std::memcpy(data_ + blob_.size_, buffer, write_size);
  blob_.size_ += write_size;
}

BufferedFileWriter::BufferedFileWriter(const std::string &path)
    : path_(path), buffer_(std::make_unique_for_overwrite<data_t[]>(FILE_BUFFER_SIZE)) {
  fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) {
    throw Exception(fmt::format("Cannot open file \"{}\" for writing: {}", path, strerror(errno)));
  }
}

BufferedFileWriter::~BufferedFileWriter() {
  if (fd_ >= 0) {
    // Best-effort flush; failures are ignored in the destructor (Sync() reports them).
    try {
      Flush();
    } catch (...) {
    }
    close(fd_);
  }
}

void BufferedFileWriter::Flush() {
  idx_t done = 0;
  while (done < offset_) {
    ssize_t n = write(fd_, buffer_.get() + done, offset_ - done);
    if (n < 0) {
      throw Exception(fmt::format("Write error in file \"{}\": {}", path_, strerror(errno)));
    }
    done += static_cast<idx_t>(n);
  }
  offset_ = 0;
}

void BufferedFileWriter::WriteData(const_data_ptr_t buffer, idx_t write_size) {
  // Large writes bypass the buffer; small ones coalesce into it.
  if (write_size >= FILE_BUFFER_SIZE) {
    Flush();
    idx_t done = 0;
    while (done < write_size) {
      ssize_t n = write(fd_, buffer + done, write_size - done);
      if (n < 0) {
        throw Exception(fmt::format("Write error in file \"{}\": {}", path_, strerror(errno)));
      }
      done += static_cast<idx_t>(n);
    }
  } else {
    if (offset_ + write_size > FILE_BUFFER_SIZE) {
      Flush();
    }
    std::memcpy(buffer_.get() + offset_, buffer, write_size);
    offset_ += write_size;
  }
  total_written_ += write_size;
}

void BufferedFileWriter::Sync() {
  Flush();
  if (fsync(fd_) != 0) {
    throw Exception(fmt::format("fsync failed for file \"{}\": {}", path_, strerror(errno)));
  }
}

}  // namespace bumblebee
