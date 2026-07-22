//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// config.h
//
// Identification: src/include/common/config.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace bumblebee {

/** Identifier of a table in the catalog. */
using table_oid_t = uint32_t;
/** Identifier of a column within a table. */
using column_oid_t = uint32_t;

/** A generic index / count, wide enough to address any row in a table. */
using idx_t = uint64_t;
/** A 64-bit hash value. */
using hash_t = uint64_t;

/** A byte of raw data. */
using data_t = uint8_t;
using data_ptr_t = data_t *;
using const_data_ptr_t = const data_t *;

/** An index into a Vector, as stored in a SelectionVector. */
using sel_t = uint32_t;

/** A date, stored as the number of days since the epoch. */
using date_t = int32_t;
/** A timestamp, stored as the number of microseconds since the epoch. Also the MVCC version stamp. */
using timestamp_t = int64_t;

/**
 * Monotonic real-time clock and its instant/interval types — used for wall-clock concerns such as the
 * transaction start time and timeout. Distinct from `timestamp_t`, which is the logical MVCC version
 * clock (not wall time). Steady (never runs backwards) so an interval is always non-negative.
 */
using steady_clock_t = std::chrono::steady_clock;
using time_point_t = steady_clock_t::time_point;
using duration_t = steady_clock_t::duration;

/**
 * How long a transaction may stay open before `TransactionManager::GarbageCollection()` aborts it.
 * Enforcement happens only when GC runs (the shell's `\gc`, or whoever schedules GC); the shell's
 * `--txn-timeout <ms>` overrides this for tests.
 */
static constexpr duration_t DEFAULT_TXN_TIMEOUT = std::chrono::hours(2);

/** Identifier of a transaction. */
using txn_id_t = int64_t;
/** Sentinel for "no transaction". */
static constexpr txn_id_t INVALID_TXN_ID = -1;
/**
 * The first transaction id, and the threshold above which a `TupleMeta::ts_` is a *temporary*
 * (uncommitted) stamp equal to the writing txn's id rather than a real commit timestamp.
 */
static constexpr txn_id_t TXN_START_ID = 1LL << 62;

/**
 * Identifier of a page in the storage layer.
 *
 * Note: this is a signed 32-bit integer, matching the on-disk format. The buffer pool hands
 * out page ids from a monotonically increasing counter, so a database that allocates more than
 * 2^31 pages over its lifetime would wrap into negative ids. At the 8 KiB page size that ceiling
 * is 16 TiB of distinct pages — well beyond this milestone's scope — so it is documented, not fixed.
 */
using page_id_t = int32_t;
/** Identifier of a frame (a page-sized slot) in the buffer pool. */
using frame_id_t = int32_t;
/** A log sequence number. Reserved for a future recovery subsystem. */
using lsn_t = int32_t;

/** The size of a page, in bytes — the unit of I/O between the buffer pool and disk. */
static constexpr int PAGE_SIZE = 8192;

/** The default number of frames in a buffer pool: 65536 frames * 8 KiB/page = 512 MiB of page data. */
static constexpr int BUFFER_POOL_SIZE = 65536;

/**
 * The default number of background worker threads the disk scheduler runs. More workers let
 * independent page I/Os proceed in parallel (the backend must be safe for concurrent, distinct-page
 * requests). Must be >= 1.
 */
static constexpr size_t DISK_SCHEDULER_WORKER_COUNT = 4;

/** The initial page capacity a single-file disk manager sizes its file for. */
static constexpr int DEFAULT_DB_IO_SIZE = 16;

/** Sentinel for "no such page". */
static constexpr page_id_t INVALID_PAGE_ID = -1;
/** Sentinel for "no such frame". */
static constexpr frame_id_t INVALID_FRAME_ID = -1;

/** The default length of a VARCHAR column when the DDL does not specify one. */
static constexpr uint32_t VARCHAR_DEFAULT_LENGTH = 128;

/**
 * The name of the auto-generated primary-key column added to a table created without an explicit PRIMARY
 * KEY. It is a reserved name (CREATE TABLE rejects a user column of this name), but a valid SQL identifier,
 * so it is usable unquoted in queries: `SELECT _id`, `WHERE _id = 5`, `ORDER BY _id`. It is a BIGINT
 * filled with a per-table auto-increment counter. Kept lowercase so it round-trips the binder's
 * case-folding of unquoted identifiers.
 */
static constexpr const char *AUTO_ID_COLUMN = "_id";

// ---------------------------------------------------------------------------------------------------
// Build-time overridable tunables.
//
// Each of the following is seeded from a `BUMBLEBEE_*` macro that defaults to the production value but
// can be overridden with a compiler `-D` (wired through the `BBDB_VECTOR_SIZE` CMake cache option for the
// vector size). This lets a *test* build lower a size so a handful of rows exercise the same multi-chunk /
// multi-partition code paths a production run only hits at scale — without turning these into runtime
// state (STANDARD_VECTOR_SIZE in particular sizes fixed-extent arrays and cannot be a runtime value).
// ---------------------------------------------------------------------------------------------------

/** The number of rows a Vector holds, i.e. the unit of work of the vectorized engine. */
#ifndef BUMBLEBEE_STANDARD_VECTOR_SIZE
#define BUMBLEBEE_STANDARD_VECTOR_SIZE 1024
#endif
static constexpr idx_t STANDARD_VECTOR_SIZE = BUMBLEBEE_STANDARD_VECTOR_SIZE;

/**
 * The size of one StringHeap chunk. It is also the largest string the heap can store,
 * since a string is never split across chunks.
 */
static constexpr idx_t MINIMUM_HEAP_SIZE = 4096 * 2;

// ---------------------------------------------------------------------------------------------------
// Execution-engine tunables (the morsel-driven push runtime — see plan_executor.md).
// ---------------------------------------------------------------------------------------------------

/**
 * The number of heap pages handed out per morsel by a parallel table scan. A morsel is the unit of
 * work one task pulls at a time; sizing it so a morsel is ≈100k rows amortizes the atomic hand-out.
 */
static constexpr idx_t MORSEL_PAGES = 64;

/** The target number of rows per columnar (row-group) morsel. */
static constexpr idx_t MORSEL_SIZE = 100000;

/** The number of radix partitions a thread-local hash table / aggregate splits into at Combine. */
#ifndef BUMBLEBEE_RADIX_PARTITIONS
#define BUMBLEBEE_RADIX_PARTITIONS 64
#endif
static constexpr idx_t RADIX_PARTITIONS = BUMBLEBEE_RADIX_PARTITIONS;

/** The default worker-thread count when the platform reports none. */
static constexpr idx_t DEFAULT_THREAD_COUNT = 16;

/** The hard upper bound on worker threads a single query may use. */
static constexpr idx_t MAX_THREADS = 128;

/** The per-query memory budget (bytes) before an in-memory breaker must spill */
static constexpr idx_t MAX_MEMORY = 256UL * 1024 * 1024;

/** The number of partitions a grace hash join splits each side into */
#ifndef BUMBLEBEE_GH_PARTITION_COUNT
#define BUMBLEBEE_GH_PARTITION_COUNT 64
#endif
static constexpr idx_t GH_PARTITION_COUNT = BUMBLEBEE_GH_PARTITION_COUNT;

/** The maximum re-partition recursion depth of a grace hash join before the NLJ fallback */
#ifndef BUMBLEBEE_GH_MAX_RECURSION
#define BUMBLEBEE_GH_MAX_RECURSION 10
#endif
static constexpr idx_t GH_MAX_RECURSION = BUMBLEBEE_GH_MAX_RECURSION;

/** The k-way fan-out of the external merge sort's merge phase */
#ifndef BUMBLEBEE_MERGE_FANOUT
#define BUMBLEBEE_MERGE_FANOUT 8
#endif
static constexpr idx_t MERGE_FANOUT = BUMBLEBEE_MERGE_FANOUT;

}  // namespace bumblebee
