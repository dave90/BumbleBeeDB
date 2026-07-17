//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// catalog.h
//
// Identification: src/include/catalog/catalog.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/schema.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/helper.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/index/b_plus_tree_index.h"
#include "storage/index/index.h"
#include "storage/row/row_operations.h"
#include "storage/table/table_heap.h"
#include "storage/table/table_storage.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** Identifier of an index in the catalog. */
using index_oid_t = uint32_t;

/**
 * The metadata BumbleBeeDB keeps about a table.
 *
 * `storage_` owns the table's physical backend (a row-format `TableHeap`, or a future columnar
 * backend). It is null when the catalog was created without a buffer pool — the SQL frontend
 * (binder, planner, optimizer) only needs the name, oid and schema, so a metadata-only catalog
 * still holds a table with no rows.
 */
struct TableInfo {
  TableInfo(Schema schema, std::string name, table_oid_t oid, std::unique_ptr<TableStorage> storage)
      : schema_{std::move(schema)}, name_{std::move(name)}, oid_{oid}, storage_{std::move(storage)} {}

  /** The table schema. */
  Schema schema_;
  /** The table name. */
  std::string name_;
  /** The table's unique identifier. */
  table_oid_t oid_;
  /** The physical storage backend, or null for a metadata-only catalog. */
  std::unique_ptr<TableStorage> storage_;
};

/** Returned by GetTable() when the table does not exist. */
static constexpr auto NULL_TABLE_INFO = nullptr;

/** The metadata BumbleBeeDB keeps about an index. */
struct IndexInfo {
  IndexInfo(Schema key_schema, std::string name, std::unique_ptr<Index> index, index_oid_t oid,
            std::string table_name)
      : key_schema_{std::move(key_schema)},
        name_{std::move(name)},
        index_{std::move(index)},
        oid_{oid},
        table_name_{std::move(table_name)} {}

  Schema key_schema_;
  std::string name_;
  std::unique_ptr<Index> index_;
  index_oid_t oid_;
  std::string table_name_;
};

/** Returned by GetIndex() when the index does not exist. */
static constexpr auto NULL_INDEX_INFO = nullptr;

/**
 * A non-persistent, in-memory catalog of table metadata.
 */
class Catalog {
 public:
  /**
   * @brief Construct a catalog.
   *
   * @param bpm The buffer pool tables are stored in. When null, the catalog is metadata-only —
   *        created tables have no storage backend (used by the SQL frontend and its tests).
   */
  explicit Catalog(BufferPoolManager *bpm = nullptr) : bpm_{bpm} {}

  /**
   * @brief Register a new table.
   *
   * @param table_name The table name.
   * @param schema The table schema.
   * @param format The physical storage format.
   * @return std::shared_ptr<TableInfo> The new table's metadata, or NULL_TABLE_INFO
   *         if a table with that name already exists.
   */
  auto CreateTable(const std::string &table_name, const Schema &schema,
                   StorageFormat format = StorageFormat::ROW) -> std::shared_ptr<TableInfo> {
    std::lock_guard lk(latch_);
    if (table_names_.find(table_name) != table_names_.end()) {
      return NULL_TABLE_INFO;
    }
    const auto table_oid = next_table_oid_++;

    std::unique_ptr<TableStorage> storage;
    if (bpm_ != nullptr) {
      switch (format) {
        case StorageFormat::ROW:
          storage = std::make_unique<TableHeap>(bpm_, std::make_shared<Schema>(schema));
          break;
        case StorageFormat::PARQUET:
          throw NotImplementedException("Parquet-backed tables are not implemented yet");
      }
    }

    auto table_info = std::make_shared<TableInfo>(schema, table_name, table_oid, std::move(storage));
    tables_.emplace(table_oid, table_info);
    table_names_.emplace(table_name, table_oid);
    return table_info;
  }

  /**
   * @brief Recovery: register a table over pages already on disk (no allocation, no re-init).
   */
  auto LoadTable(table_oid_t oid, const std::string &table_name, const Schema &schema, page_id_t first_page_id,
                 page_id_t last_page_id, StorageFormat format) -> std::shared_ptr<TableInfo> {
    std::lock_guard lk(latch_);
    std::unique_ptr<TableStorage> storage;
    if (bpm_ != nullptr) {
      switch (format) {
        case StorageFormat::ROW:
          storage =
              std::make_unique<TableHeap>(bpm_, std::make_shared<Schema>(schema), first_page_id, last_page_id);
          break;
        case StorageFormat::PARQUET:
          throw NotImplementedException("Parquet-backed tables are not implemented yet");
      }
    }
    auto table_info = std::make_shared<TableInfo>(schema, table_name, oid, std::move(storage));
    tables_.emplace(oid, table_info);
    table_names_.emplace(table_name, oid);
    return table_info;
  }

  /**
   * @brief Look a table up by name.
   *
   * @param table_name The table name.
   * @return std::shared_ptr<TableInfo> The table's metadata, or NULL_TABLE_INFO.
   */
  auto GetTable(const std::string &table_name) const -> std::shared_ptr<TableInfo> {
    std::lock_guard lk(latch_);
    auto table_oid = table_names_.find(table_name);
    if (table_oid == table_names_.end()) {
      return NULL_TABLE_INFO;
    }
    return GetTable(table_oid->second);
  }

  /**
   * @brief Look a table up by oid.
   *
   * @param table_oid The table identifier.
   * @return std::shared_ptr<TableInfo> The table's metadata, or NULL_TABLE_INFO.
   */
  auto GetTable(table_oid_t table_oid) const -> std::shared_ptr<TableInfo> {
    std::lock_guard lk(latch_);
    auto meta = tables_.find(table_oid);
    if (meta == tables_.end()) {
      return NULL_TABLE_INFO;
    }
    return meta->second;
  }

  /** @return The names of every table, in no particular order. */
  auto GetTableNames() const -> std::vector<std::string> {
    std::lock_guard lk(latch_);
    std::vector<std::string> result;
    result.reserve(table_names_.size());
    for (const auto &[name, _] : table_names_) {
      result.push_back(name);
    }
    return result;
  }

  /**
   * @brief Create a B+ tree index and populate it from the existing (live) rows of a table.
   *
   * @tparam KeySize The fixed key size; must be >= the key schema's inlined width.
   * @param index_name The index name.
   * @param table_name The table to index.
   * @param key_attrs The base-table column indices that make up the key.
   * @return The new index's metadata, or NULL_INDEX_INFO if the table is missing / has no storage.
   */
  template <size_t KeySize>
  auto CreateIndex(const std::string &index_name, const std::string &table_name,
                   const std::vector<uint32_t> &key_attrs) -> std::shared_ptr<IndexInfo> {
    std::lock_guard lk(latch_);
    auto table_info = GetTable(table_name);
    if (table_info == NULL_TABLE_INFO || table_info->storage_ == nullptr) {
      return NULL_INDEX_INFO;
    }

    auto key_schema = Schema::CopySchema(&table_info->schema_, key_attrs);
    auto metadata = std::make_unique<IndexMetadata>(index_name, table_name, key_schema, key_attrs);
    auto index = std::make_unique<BPlusTreeIndex<KeySize>>(std::move(metadata), bpm_);

    // Populate from the table. The scan already skips logically deleted rows, so the index never
    // gets a stale key pointing at a tombstoned RID (bug #7).
    auto key_width = key_schema.GetInlinedStorageSize();

    // Precompute, once, each key column's source chunk column, destination byte offset, and physical
    // type — the fixed recipe the vectorized key gather uses for every chunk.
    std::vector<idx_t> dst_offsets;
    std::vector<PhysicalType> key_types;
    dst_offsets.reserve(key_attrs.size());
    key_types.reserve(key_attrs.size());
    for (uint32_t k = 0; k < key_attrs.size(); k++) {
      dst_offsets.push_back(key_schema.GetColumn(k).GetOffset());
      key_types.push_back(key_schema.GetColumn(k).GetType().GetPhysicalType());
    }

    auto scan = table_info->storage_->MakeScan();
    DataChunk chunk;
    chunk.Initialize(TableColumnTypes(table_info->schema_));
    Vector row_ids{LogicalType{LogicalTypeId::BIGINT}};
    std::vector<data_t> keys;  // packed (count * KeySize) key buffer, reused per chunk
    while (scan->Next(chunk, &row_ids)) {
      auto count = chunk.GetSize();
      auto rid_data = FlatVector::GetData<int64_t>(row_ids);

      // Vectorized key gather: build every key of the chunk in one templated pass, no per-cell Value
      // boxing. `assign` zeroes the buffer so key padding beyond the schema width stays deterministic.
      keys.assign(count * KeySize, 0);
      RowOperations::ScatterKeys(chunk, key_attrs, dst_offsets, key_types, keys.data(), KeySize, count);

      // TODO(bulk-load): the B+ tree insert is still scalar — one root-to-leaf descent (with latch
      // crabbing) per key. The key GATHER above is now vectorized; the INSERT remains the row-at-a-time
      // bottleneck. Replace this loop with a vectorized bulk-load (BPlusTree::BulkLoad): sort the
      // gathered (key, RID) batch by the comparator, then build packed leaves left-to-right and the
      // internal levels bottom-up, instead of N independent descents-with-splits.
      for (idx_t row = 0; row < count; row++) {
        index->InsertEntry(keys.data() + row * KeySize, key_width, RID(rid_data[row]));
      }
    }

    const auto index_oid = next_index_oid_++;
    auto index_info = std::make_shared<IndexInfo>(key_schema, index_name, std::move(index), index_oid, table_name);
    indexes_.emplace(index_oid, index_info);
    index_names_[table_name].emplace(index_name, index_oid);
    return index_info;
  }

  /**
   * @brief Recovery: reopen an existing B+ tree index over its persisted header page (no rebuild).
   *
   * @tparam KeySize The persisted key size; the Database dispatches to the right instantiation.
   */
  template <size_t KeySize>
  auto LoadIndex(index_oid_t oid, const std::string &index_name, const std::string &table_name,
                 const std::vector<uint32_t> &key_attrs, const Schema &key_schema, page_id_t header_page_id)
      -> std::shared_ptr<IndexInfo> {
    std::lock_guard lk(latch_);
    auto metadata = std::make_unique<IndexMetadata>(index_name, table_name, key_schema, key_attrs);
    auto index = std::make_unique<BPlusTreeIndex<KeySize>>(std::move(metadata), bpm_, header_page_id);
    auto index_info = std::make_shared<IndexInfo>(key_schema, index_name, std::move(index), oid, table_name);
    indexes_.emplace(oid, index_info);
    index_names_[table_name].emplace(index_name, oid);
    return index_info;
  }

  /** @brief Look an index up by name and table. */
  auto GetIndex(const std::string &index_name, const std::string &table_name) const -> std::shared_ptr<IndexInfo> {
    std::lock_guard lk(latch_);
    auto table_it = index_names_.find(table_name);
    if (table_it == index_names_.end()) {
      return NULL_INDEX_INFO;
    }
    auto idx_it = table_it->second.find(index_name);
    if (idx_it == table_it->second.end()) {
      return NULL_INDEX_INFO;
    }
    auto info = indexes_.find(idx_it->second);
    return info == indexes_.end() ? NULL_INDEX_INFO : info->second;
  }

  /** @return A snapshot of every table's metadata (for persisting the catalog). */
  auto GetTables() const -> std::vector<std::shared_ptr<TableInfo>> {
    std::lock_guard lk(latch_);
    std::vector<std::shared_ptr<TableInfo>> out;
    out.reserve(tables_.size());
    for (const auto &[oid, info] : tables_) {
      out.push_back(info);
    }
    return out;
  }

  /** @return A snapshot of every index's metadata (for persisting the catalog). */
  auto GetIndexes() const -> std::vector<std::shared_ptr<IndexInfo>> {
    std::lock_guard lk(latch_);
    std::vector<std::shared_ptr<IndexInfo>> out;
    out.reserve(indexes_.size());
    for (const auto &[oid, info] : indexes_) {
      out.push_back(info);
    }
    return out;
  }

  auto GetNextTableOid() const -> table_oid_t { return next_table_oid_.load(); }
  auto GetNextIndexOid() const -> index_oid_t { return next_index_oid_.load(); }

  /** @brief Recovery: restore the oid allocators so new DDL does not reuse a persisted oid. */
  void SetNextOids(table_oid_t next_table_oid, index_oid_t next_index_oid) {
    next_table_oid_.store(next_table_oid);
    next_index_oid_.store(next_index_oid);
  }

 private:
  /** @return The logical types of a schema's columns, for initializing a scan chunk. */
  static auto TableColumnTypes(const Schema &schema) -> std::vector<LogicalType> {
    std::vector<LogicalType> types;
    types.reserve(schema.GetColumnCount());
    for (const auto &col : schema.GetColumns()) {
      types.push_back(col.GetType());
    }
    return types;
  }

  /** The buffer pool tables are stored in; null for a metadata-only catalog. Non-owning. */
  BufferPoolManager *bpm_{nullptr};

  /**
   * Serializes catalog mutations and lookups. Recursive so CreateIndex (which holds the latch) can
   * call GetTable. DDL is rare, so a single coarse latch is enough.
   */
  mutable std::recursive_mutex latch_;

  /** Table oid -> metadata. */
  std::unordered_map<table_oid_t, std::shared_ptr<TableInfo>> tables_;

  /** Table name -> table oid. */
  std::unordered_map<std::string, table_oid_t> table_names_;

  /** Index oid -> metadata. */
  std::unordered_map<index_oid_t, std::shared_ptr<IndexInfo>> indexes_;

  /** Table name -> {index name -> index oid}. */
  std::unordered_map<std::string, std::unordered_map<std::string, index_oid_t>> index_names_;

  /** The oid the next created table gets. */
  std::atomic<table_oid_t> next_table_oid_{0};

  /** The oid the next created index gets. */
  std::atomic<index_oid_t> next_index_oid_{0};
};

}  // namespace bumblebee
