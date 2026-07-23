//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_manifest.cpp
//
// Identification: src/storage/parquet/parquet_manifest.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_manifest.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "common/exception.h"
#include "fmt/format.h"

namespace bumblebee {

namespace fs = std::filesystem;

auto ParquetManifestIO::ReadLatest(const std::string &dir) -> std::optional<ParquetManifest> {
  int64_t best_version = -1;
  fs::path best_path;
  const std::string prefix = MANIFEST_PREFIX;
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    auto name = entry.path().filename().string();
    if (!name.starts_with(prefix)) {
      continue;
    }
    // A leftover temp file (crashed commit) never has a pure-numeric suffix and is skipped.
    auto suffix = name.substr(prefix.size());
    if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), [](char c) { return isdigit(c) != 0; })) {
      continue;
    }
    auto version = std::stoll(suffix);
    if (version > best_version) {
      best_version = version;
      best_path = entry.path();
    }
  }
  if (best_version < 0) {
    return std::nullopt;
  }

  std::ifstream in(best_path);
  if (!in.is_open()) {
    throw Exception(fmt::format("cannot open manifest '{}'", best_path.string()));
  }
  std::string header;
  std::getline(in, header);
  if (!header.starts_with("bbdb-manifest")) {
    throw Exception(fmt::format("corrupt manifest '{}': bad header", best_path.string()));
  }

  ParquetManifest manifest;
  manifest.version_ = best_version;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto tab = line.find('\t');
    if (tab == std::string::npos) {
      throw Exception(fmt::format("corrupt manifest '{}': bad entry '{}'", best_path.string(), line));
    }
    ManifestEntry entry;
    entry.row_count_ = static_cast<idx_t>(std::stoll(line.substr(0, tab)));
    entry.file_name_ = line.substr(tab + 1);
    manifest.entries_.push_back(std::move(entry));
  }
  return manifest;
}

auto ParquetManifestIO::ListParquetFiles(const std::string &dir) -> std::vector<std::string> {
  std::vector<std::string> files;
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    auto name = entry.path().filename().string();
    if (name.ends_with(".parquet") && !name.starts_with("_") && !name.starts_with(".")) {
      files.push_back(name);
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void ParquetManifestIO::Write(const std::string &dir, const ParquetManifest &manifest) {
  auto final_path = fs::path(dir) / fmt::format("{}{}", MANIFEST_PREFIX, manifest.version_);
  auto tmp_path = fs::path(dir) / fmt::format("{}{}.tmp", MANIFEST_PREFIX, manifest.version_);

  std::ostringstream body;
  body << "bbdb-manifest 1\n";
  for (const auto &e : manifest.entries_) {
    body << e.row_count_ << '\t' << e.file_name_ << '\n';
  }
  const auto payload = body.str();

  int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw Exception(fmt::format("cannot write manifest '{}': {}", tmp_path.string(), strerror(errno)));
  }
  size_t done = 0;
  while (done < payload.size()) {
    ssize_t n = write(fd, payload.data() + done, payload.size() - done);
    if (n < 0) {
      close(fd);
      throw Exception(fmt::format("cannot write manifest '{}': {}", tmp_path.string(), strerror(errno)));
    }
    done += static_cast<size_t>(n);
  }
  // fsync before the rename: the commit point must find the bytes durable.
  if (fsync(fd) != 0) {
    close(fd);
    throw Exception(fmt::format("fsync failed for manifest '{}': {}", tmp_path.string(), strerror(errno)));
  }
  close(fd);

  // The atomic rename is the commit point.
  if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    throw Exception(fmt::format("cannot commit manifest '{}': {}", final_path.string(), strerror(errno)));
  }
}

}  // namespace bumblebee
