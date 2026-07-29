//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// index_maintenance.cpp
//
// Identification: src/execution/operator/persistent/index_maintenance.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/operator/persistent/index_maintenance.h"

#include <cstring>
#include <optional>
#include <vector>

#include "common/exception.h"
#include "concurrency/transaction.h"
#include "fmt/format.h"
#include "storage/index/index.h"
#include "storage/mvcc/mvcc.h"
#include "storage/row/row_operations.h"
#include "storage/table/table_heap.h"

namespace bumblebee {

namespace {

/** A chunk's packed key bytes plus the stride and inlined width the index reads them at. */
struct GatheredKeys {
  std::vector<data_t> bytes;  // count * stride, zero-padded
  size_t stride;
  uint32_t width;

  auto KeyAt(idx_t row) -> data_ptr_t { return bytes.data() + row * stride; }
};

/** @brief Gather every row's key bytes with the same `ScatterKeys` recipe `CreateIndex` uses. */
auto GatherKeys(IndexInfo &idx_info, DataChunk &chunk) -> GatheredKeys {
  auto *index = idx_info.index_.get();
  const auto &key_schema = idx_info.key_schema_;
  const auto &key_attrs = index->GetMetadata()->GetKeyAttrs();

  std::vector<idx_t> dst_offsets;
  std::vector<PhysicalType> key_types;
  dst_offsets.reserve(key_attrs.size());
  key_types.reserve(key_attrs.size());
  for (uint32_t k = 0; k < key_attrs.size(); k++) {
    dst_offsets.push_back(key_schema.GetColumn(k).GetOffset());
    key_types.push_back(key_schema.GetColumn(k).GetType().GetPhysicalType());
  }

  GatheredKeys out;
  out.stride = index->GetKeySize();
  out.width = key_schema.GetInlinedStorageSize();
  const idx_t count = chunk.GetSize();
  out.bytes.assign(count * out.stride, 0);
  RowOperations::ScatterKeys(chunk, key_attrs, dst_offsets, key_types, out.bytes.data(), out.stride, count);
  return out;
}

}  // namespace

auto FindPrimaryKeyIndex(Catalog &catalog, const TableInfo &info) -> std::shared_ptr<IndexInfo> {
  for (auto &idx : catalog.GetTableIndexes(info.name_)) {
    if (idx->index_->GetMetadata()->GetKeyAttrs() == info.pk_attrs_) {
      return idx;
    }
  }
  return NULL_INDEX_INFO;
}

void InsertOrRevive(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, TableHeap &heap, IndexInfo &pk,
                    DataChunk &chunk, Vector &out_rids, bool keys_are_fresh) {
  const idx_t count = chunk.GetSize();
  if (count == 0) {
    return;
  }
  auto keys = GatherKeys(pk, chunk);
  auto scattered = heap.ScatterChunk(chunk);
  auto *index = pk.index_.get();
  const auto temp_ts = txn->GetTransactionTempTs();
  auto *rid_out = FlatVector::GetData<int64_t>(out_rids);

  std::vector<RID> found;  // reused across rows
  for (idx_t i = 0; i < count; i++) {
    auto *key = keys.KeyAt(i);
    found.clear();
    if (!keys_are_fresh) {
      index->ScanKey(key, keys.width, &found);
    }
    if (found.empty()) {
      // A key the directory has never seen: append a fresh tuple and register it.
      RID rid = heap.AppendRowBytes(TupleMeta{temp_ts, false}, scattered.RowAt(i),
                                    static_cast<uint16_t>(scattered.sizes[i]));
      txn->AppendWriteSet(oid, rid);
      index->InsertEntry(key, keys.width, rid);
      rid_out[i] = rid.Get();
      continue;
    }
    // The directory already points somewhere for this key; the tuple decides whether it is a duplicate.
    const RID rid = found.front();
    if (CollectVisibleVersion(txn_mgr, txn, heap, rid).has_value()) {
      throw ExecutionException(fmt::format("duplicate key violates primary key '{}'", pk.name_));
    }
    // Deleted / not-visible-to-me: revive the slot in place (ApplyMvccModify handles write-write conflicts
    // and versions the deleted pre-image, so an abort tombstones it again).
    ApplyMvccModify(txn_mgr, txn, oid, heap, rid, /*is_delete=*/false, scattered.RowAt(i),
                    static_cast<uint16_t>(scattered.sizes[i]));
    rid_out[i] = rid.Get();
  }
}

auto PrimaryKeyChanged(IndexInfo &pk, DataChunk &old_chunk, DataChunk &new_chunk) -> std::vector<bool> {
  auto old_keys = GatherKeys(pk, old_chunk);
  auto new_keys = GatherKeys(pk, new_chunk);
  const idx_t count = new_chunk.GetSize();
  std::vector<bool> changed(count);
  for (idx_t i = 0; i < count; i++) {
    changed[i] = std::memcmp(old_keys.KeyAt(i), new_keys.KeyAt(i), new_keys.width) != 0;
  }
  return changed;
}

}  // namespace bumblebee
