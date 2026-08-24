//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database_config.h
//
// Identification: src/include/main/database_config.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>

#include "common/config.h"

namespace bumblebee {

/**
 * @brief Immutable-at-runtime configuration shared by every connection and query in a database.
 *
 * Frontends assemble this value before opening the database. `DatabaseInstance` stores one copy and
 * each statement snapshots the execution fields into its `ClientContext`.
 */
struct DatabaseConfig {
  /** Database-wide native worker limit; zero selects an automatic hardware-derived limit. */
  idx_t worker_threads_{0};
  /** Database-wide query-memory ceiling in bytes. */
  idx_t max_memory_{MAX_MEMORY};
  /** Durable/in-memory buffer-pool frame count. */
  size_t frames_{BUFFER_POOL_SIZE};
  /** Transaction lifetime before timeout cancellation. */
  duration_t transaction_timeout_{DEFAULT_TXN_TIMEOUT};
  /** Prefer the external join/sort implementations before an in-memory overflow. */
  bool prefer_external_{false};
  /** Heap pages assigned to one parallel scan morsel. */
  idx_t morsel_pages_{MORSEL_PAGES};
  /** Reserved target rows per columnar morsel. */
  idx_t morsel_size_{MORSEL_SIZE};
  /** Test/advanced override for hash-aggregate partitioning; zero uses the operator default. */
  idx_t aggregate_partition_threshold_{0};
};

}  // namespace bumblebee
