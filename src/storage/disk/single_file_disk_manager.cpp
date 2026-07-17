//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// single_file_disk_manager.cpp
//
// Identification: src/storage/disk/single_file_disk_manager.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/disk/single_file_disk_manager.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>  // NOLINT
#include <string>

#include "common/exception.h"
#include "common/logger.h"

namespace bumblebee {

SingleFileDiskManager::SingleFileDiskManager(const std::filesystem::path &db_file) : db_file_name_(db_file) {
  std::scoped_lock lock(db_io_latch_);
  db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
  if (!db_io_.is_open()) {
    // The file does not exist yet: create it.
    db_io_.clear();
    db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out | std::ios::in);
    if (!db_io_.is_open()) {
      throw Exception("can't open db file");
    }
  }

  // Adopt an existing file's size so a reopened database is NOT truncated (positional offsets mean the
  // file already holds every page N at N*PAGE_SIZE). Grow to the default minimum, but never shrink.
  auto file_size = GetFileSize(db_file_name_);
  size_t existing_pages = file_size > 0 ? static_cast<size_t>(file_size) / PAGE_SIZE : 0;
  page_capacity_ = std::max<size_t>(DEFAULT_DB_IO_SIZE, existing_pages);
  if (file_size < 0 || static_cast<size_t>(file_size) < page_capacity_ * PAGE_SIZE) {
    std::filesystem::resize_file(db_file, page_capacity_ * PAGE_SIZE);
  }
}

SingleFileDiskManager::~SingleFileDiskManager() { ShutDown(); }

void SingleFileDiskManager::ShutDown() {
  std::scoped_lock lock(db_io_latch_);
  if (db_io_.is_open()) {
    db_io_.close();
  }
}

void SingleFileDiskManager::EnsureCapacity(page_id_t page_id) {
  auto needed = static_cast<size_t>(page_id) + 1;  // pages [0, page_id] must fit
  if (needed <= page_capacity_) {
    return;
  }
  while (page_capacity_ < needed) {
    page_capacity_ *= 2;
  }
  std::filesystem::resize_file(db_file_name_, page_capacity_ * PAGE_SIZE);
}

auto SingleFileDiskManager::WritePage(page_id_t page_id, const_data_ptr_t page_data) -> bool {
  std::scoped_lock lock(db_io_latch_);
  if (page_id < 0) {
    return false;
  }
  EnsureCapacity(page_id);
  auto offset = static_cast<size_t>(page_id) * PAGE_SIZE;  // positional: page N at N*PAGE_SIZE

  db_io_.seekp(static_cast<std::streamoff>(offset));
  db_io_.write(reinterpret_cast<const char *>(page_data), PAGE_SIZE);  // fstream speaks char at the leaf
  db_io_.flush();
  if (db_io_.bad()) {
    LOG_DEBUG("I/O error while writing page %d", page_id);
    db_io_.clear();
    return false;
  }
  num_writes_ += 1;
  return true;
}

auto SingleFileDiskManager::ReadPage(page_id_t page_id, data_ptr_t page_data) -> bool {
  std::scoped_lock lock(db_io_latch_);
  if (page_id < 0) {
    std::memset(page_data, 0, PAGE_SIZE);
    return false;
  }
  auto offset = static_cast<size_t>(page_id) * PAGE_SIZE;

  int file_size = GetFileSize(db_file_name_);
  if (file_size < 0) {
    LOG_DEBUG("I/O error: fail to get db file size");
    return false;
  }
  if (offset + PAGE_SIZE > static_cast<size_t>(file_size)) {
    // A never-written page (beyond the current file) reads back as zeros. A read never allocates.
    std::memset(page_data, 0, PAGE_SIZE);
    return true;
  }

  db_io_.seekg(static_cast<std::streamoff>(offset));
  db_io_.read(reinterpret_cast<char *>(page_data), PAGE_SIZE);
  if (db_io_.bad()) {
    LOG_DEBUG("I/O error while reading page %d", page_id);
    db_io_.clear();
    return false;
  }

  // Zero-fill if the file ended before a full page was read.
  int read_count = static_cast<int>(db_io_.gcount());
  if (read_count < PAGE_SIZE) {
    db_io_.clear();
    std::memset(page_data + read_count, 0, PAGE_SIZE - read_count);
  }
  return true;
}

auto SingleFileDiskManager::DeletePage(page_id_t page_id) -> void {
  std::scoped_lock lock(db_io_latch_);
  num_deletes_ += 1;
  if (page_id < 0) {
    return;
  }
  auto offset = static_cast<size_t>(page_id) * PAGE_SIZE;
  int file_size = GetFileSize(db_file_name_);
  if (file_size < 0 || offset + PAGE_SIZE > static_cast<size_t>(file_size)) {
    return;  // never written / beyond EOF — nothing on disk to clear (idempotent)
  }
  // Zero-fill the slot so a subsequent read of this page sees an empty page. The id itself is
  // reclaimed for reuse by the buffer pool's free list, not here.
  std::array<char, PAGE_SIZE> zeros{};
  db_io_.seekp(static_cast<std::streamoff>(offset));
  db_io_.write(zeros.data(), PAGE_SIZE);
  db_io_.flush();
  if (db_io_.bad()) {
    LOG_DEBUG("I/O error while deleting page %d", page_id);
    db_io_.clear();
  }
}

auto SingleFileDiskManager::GetDbFileSize() -> size_t {
  auto file_size = GetFileSize(db_file_name_);
  if (file_size < 0) {
    LOG_DEBUG("I/O error: fail to get db file size");
    return 0;
  }
  return static_cast<size_t>(file_size);
}

auto SingleFileDiskManager::GetFileSize(const std::string &file_name) -> int {
  struct stat stat_buf {};
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? static_cast<int>(stat_buf.st_size) : -1;
}

}  // namespace bumblebee
