//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// resolve_table_storage.h
//
// Identification: src/include/execution/operator/resolve_table_storage.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string_view>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "fmt/format.h"
#include "main/client_context.h"

namespace bumblebee {

/**
 * @brief Resolve a table oid to its concrete storage backend, or throw.
 *
 * Every operator bound to one storage flavour opens the same way: look the table up, check it has a
 * backend at all, then down-cast to the flavour it can actually drive (TableHeap for the row-format
 * operators, ParquetTable for the external ones). Runs once per operator state, never per row.
 *
 * @tparam STORAGE The expected backend type.
 * @param context The client context holding the catalog.
 * @param oid The table to resolve.
 * @param op_name Operator name, used as the error-message prefix.
 * @param kind_desc How to describe STORAGE in the mismatch message, e.g. "a row-format heap".
 * @return STORAGE* The backend, never null.
 */
template <class STORAGE>
auto ResolveTableStorage(ClientContext &context, table_oid_t oid, std::string_view op_name,
                         std::string_view kind_desc) -> STORAGE * {
  auto info = context.catalog_.GetTable(oid);
  if (info == NULL_TABLE_INFO || info->storage_ == nullptr) {
    throw ExecutionException(fmt::format("{}: table has no storage backend", op_name));
  }
  auto *storage = dynamic_cast<STORAGE *>(info->storage_.get());
  if (storage == nullptr) {
    throw ExecutionException(fmt::format("{}: table is not {}", op_name, kind_desc));
  }
  return storage;
}

}  // namespace bumblebee
