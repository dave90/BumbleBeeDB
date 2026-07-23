//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_table_ops.cpp
//
// Identification: src/storage/parquet/parquet_table_ops.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_table_ops.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>

#include "common/exception.h"
#include "fmt/format.h"
#include "storage/parquet/parquet_writer.h"
#include "storage/table/parquet_table.h"

namespace bumblebee {

namespace fs = std::filesystem;

ExternalWriteGuard::ExternalWriteGuard(ParquetTable &table, const std::string &table_name) : table_(table) {
  if (!table_.TryLockForWrite()) {
    throw ExecutionException(fmt::format("concurrent modification of external table '{}'", table_name));
  }
  process_locked_ = true;

  lock_path_ = (fs::path(table_.GetPath()) / ParquetManifestIO::LOCK_FILE).string();
  int fd = open(lock_path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0 && errno == EEXIST) {
    // A lock file left by a crashed process is replaced after the staleness threshold; a fresh one
    // means another process is writing right now.
    struct stat st{};
    if (stat(lock_path_.c_str(), &st) == 0 && (time(nullptr) - st.st_mtime) > STALE_LOCK_SECONDS) {
      unlink(lock_path_.c_str());
      fd = open(lock_path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    }
  }
  if (fd < 0) {
    table_.UnlockWrite();
    process_locked_ = false;
    throw ExecutionException(fmt::format("concurrent modification of external table '{}'", table_name));
  }
  close(fd);
  file_locked_ = true;
}

ExternalWriteGuard::~ExternalWriteGuard() {
  if (file_locked_) {
    unlink(lock_path_.c_str());
  }
  if (process_locked_) {
    table_.UnlockWrite();
  }
}

auto SchemaLogicalTypes(const Schema &schema) -> std::vector<LogicalType> {
  std::vector<LogicalType> types;
  types.reserve(schema.GetColumnCount());
  for (const auto &c : schema.GetColumns()) {
    types.push_back(c.GetType());
  }
  return types;
}

auto GeneratePartFileName(int64_t version) -> std::string {
  // Uniqueness comes from the random suffix; the version prefix keeps a folder listing readable.
  static std::random_device rd;
  std::uniform_int_distribution<uint64_t> dist;
  return fmt::format("part-{:05}-{:016x}.parquet", version, dist(rd));
}

PartFileWriter::PartFileWriter(std::string dir, std::string file_name, const Schema &schema)
    : dir_(std::move(dir)), file_name_(std::move(file_name)), types_(SchemaLogicalTypes(schema)) {
  names_.reserve(schema.GetColumnCount());
  for (const auto &c : schema.GetColumns()) {
    names_.push_back(c.GetName());
  }
  auto full_path = (fs::path(dir_) / file_name_).string();
  writer_ = std::make_unique<ParquetWriter>(full_path, types_, names_, format::CompressionCodec::SNAPPY);
}

PartFileWriter::~PartFileWriter() {
  // An abandoned writer (an error mid-rewrite) leaves an orphan file the manifest never
  // references — harmless by construction.
}

void PartFileWriter::FlushGroup() {
  writer_->Flush(pending_);
  pending_.Reset();
}

void PartFileWriter::Append(DataChunk &chunk) {
  if (chunk.GetSize() == 0) {
    return;
  }
  pending_.Append(chunk);
  total_rows_ += chunk.GetSize();
  if (pending_.GetCount() >= ROWS_PER_ROW_GROUP) {
    FlushGroup();
  }
}

auto PartFileWriter::Finish() -> ManifestEntry {
  BUMBLEBEE_ASSERT(!finished_, "PartFileWriter::Finish called twice");
  finished_ = true;
  FlushGroup();
  writer_->Finalize();
  return ManifestEntry{file_name_, total_rows_};
}

auto WritePartFile(const std::string &dir, const std::string &file_name, const Schema &schema,
                   ChunkCollection &rows) -> ManifestEntry {
  PartFileWriter writer(dir, file_name, schema);
  for (auto &chunk : rows.Chunks()) {
    writer.Append(*chunk);
  }
  return writer.Finish();
}

}  // namespace bumblebee
