//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_manifest.h
//
// Identification: src/include/storage/parquet/parquet_manifest.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/config.h"

namespace bumblebee {

/** One live data file of an external table. */
struct ManifestEntry {
  std::string file_name_;  // relative to the table folder
  idx_t row_count_{0};
};

/**
 * @brief The commit protocol of an external parquet table.
 *
 * The newest `_bbdb_manifest.N` in the table folder lists the live part files; files present in
 * the directory but absent from the manifest are invisible. A writer commits by writing manifest
 * N+1 to a temp name and atomically renaming it into place — so readers always see either the old
 * or the new state, and a crashed half-finished rewrite is a no-op (orphan part files only).
 */
struct ParquetManifest {
  int64_t version_{-1};  // -1: no manifest on disk yet
  std::vector<ManifestEntry> entries_;

  auto TotalRows() const -> idx_t {
    idx_t total = 0;
    for (const auto &e : entries_) {
      total += e.row_count_;
    }
    return total;
  }
};

class ParquetManifestIO {
 public:
  static constexpr const char *MANIFEST_PREFIX = "_bbdb_manifest.";
  static constexpr const char *LOCK_FILE = "_bbdb_lock";

  /**
   * @brief Read the newest `_bbdb_manifest.N` in `dir`.
   *
   * @param dir The table folder.
   * @return The parsed manifest, or nullopt when none exists.
   */
  static auto ReadLatest(const std::string &dir) -> std::optional<ParquetManifest>;

  /** @brief List the `*.parquet` files of `dir` in name order (adoption of a foreign folder). */
  static auto ListParquetFiles(const std::string &dir) -> std::vector<std::string>;

  /**
   * @brief Commit `manifest` as version `manifest.version_`: write to a temp name, fsync, and
   * atomically rename into place.
   */
  static void Write(const std::string &dir, const ParquetManifest &manifest);
};

}  // namespace bumblebee
