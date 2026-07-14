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
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/schema.h"
#include "common/config.h"

namespace bumblebee {

/**
 * The metadata BumbleBeeDB keeps about a table.
 *
 * There is deliberately no TableHeap here. The SQL frontend — binder, planner,
 * optimizer — only ever needs a table's name, oid and schema, so the catalog does
 * not depend on the storage layer at all. When row-format storage lands, TableInfo
 * gains an owning pointer to it and nothing above the catalog has to change.
 */
struct TableInfo {
  TableInfo(Schema schema, std::string name, table_oid_t oid)
      : schema_{std::move(schema)}, name_{std::move(name)}, oid_{oid} {}

  /** The table schema. */
  Schema schema_;
  /** The table name. */
  std::string name_;
  /** The table's unique identifier. */
  table_oid_t oid_;
};

/** Returned by GetTable() when the table does not exist. */
static constexpr auto NULL_TABLE_INFO = nullptr;

/**
 * A non-persistent, in-memory catalog of table metadata.
 */
class Catalog {
 public:
  Catalog() = default;

  /**
   * @brief Register a new table.
   *
   * @param table_name The table name.
   * @param schema The table schema.
   * @return std::shared_ptr<TableInfo> The new table's metadata, or NULL_TABLE_INFO
   *         if a table with that name already exists.
   */
  auto CreateTable(const std::string &table_name, const Schema &schema) -> std::shared_ptr<TableInfo> {
    if (table_names_.find(table_name) != table_names_.end()) {
      return NULL_TABLE_INFO;
    }
    const auto table_oid = next_table_oid_++;
    auto table_info = std::make_shared<TableInfo>(schema, table_name, table_oid);
    tables_.emplace(table_oid, table_info);
    table_names_.emplace(table_name, table_oid);
    return table_info;
  }

  /**
   * @brief Look a table up by name.
   *
   * @param table_name The table name.
   * @return std::shared_ptr<TableInfo> The table's metadata, or NULL_TABLE_INFO.
   */
  auto GetTable(const std::string &table_name) const -> std::shared_ptr<TableInfo> {
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
    auto meta = tables_.find(table_oid);
    if (meta == tables_.end()) {
      return NULL_TABLE_INFO;
    }
    return meta->second;
  }

  /** @return The names of every table, in no particular order. */
  auto GetTableNames() const -> std::vector<std::string> {
    std::vector<std::string> result;
    result.reserve(table_names_.size());
    for (const auto &[name, _] : table_names_) {
      result.push_back(name);
    }
    return result;
  }

 private:
  /** Table oid -> metadata. */
  std::unordered_map<table_oid_t, std::shared_ptr<TableInfo>> tables_;

  /** Table name -> table oid. */
  std::unordered_map<std::string, table_oid_t> table_names_;

  /** The oid the next created table gets. */
  std::atomic<table_oid_t> next_table_oid_{0};
};

}  // namespace bumblebee
