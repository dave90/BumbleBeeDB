//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// tuple_meta.h
//
// Identification: src/include/storage/table/tuple_meta.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"

namespace bumblebee {

/**
 * @brief Per-row metadata stored alongside a row's bytes in a table page.
 *
 * `is_deleted_` marks a logically deleted (tombstoned) row: its slot number stays stable so any
 * index entry pointing at it remains valid, but scans skip it.
 */
struct TupleMeta {
  /** A timestamp, reserved for future MVCC / recovery use. */
  timestamp_t ts_;
  /** Whether this row has been logically deleted. */
  bool is_deleted_;
};

}  // namespace bumblebee
