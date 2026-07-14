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
/** A timestamp, stored as the number of microseconds since the epoch. */
using timestamp_t = int64_t;

/** The default length of a VARCHAR column when the DDL does not specify one. */
static constexpr uint32_t VARCHAR_DEFAULT_LENGTH = 128;

/** The number of rows a Vector holds, i.e. the unit of work of the vectorized engine. */
static constexpr idx_t STANDARD_VECTOR_SIZE = 1024;

/**
 * The size of one StringHeap chunk. It is also the largest string the heap can store,
 * since a string is never split across chunks.
 */
static constexpr idx_t MINIMUM_HEAP_SIZE = 4096 * 2;

}  // namespace bumblebee
