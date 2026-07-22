//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database.cpp
//
// Identification: src/database.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "database.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/schema_codec.h"
#include "common/exception.h"
#include "storage/table/table_heap.h"

namespace bumblebee {

Database::Database(const std::filesystem::path &db_file, size_t num_frames, duration_t txn_timeout)
    : num_frames_(num_frames) {
  dm_ = std::make_unique<SingleFileDiskManager>(db_file);

  std::vector<char> record;
  if (ReadCatalogChain(record)) {
    // Existing database: reconstruct the allocator + catalog from the assembled record.
    ByteReader r(record.data(), record.size());
    LoadCatalog(r);
  } else {
    // Fresh database: seed a zero-based allocator, reserve page 0 for the catalog, empty catalog.
    bpm_ = std::make_unique<BufferPoolManager>(num_frames_, dm_.get());
    auto root = bpm_->NewPage();
    BUMBLEBEE_ASSERT(root == kCatalogRootPage, "the catalog root must be the first allocated page (0)");
    (void)root;
    catalog_pages_ = {kCatalogRootPage};
    catalog_ = std::make_unique<Catalog>(bpm_.get());
  }

  // The transaction manager is per-database and resolves oids through this catalog. Its timeout
  // (default 2 hours) is enforced whenever GarbageCollection() is driven.
  txn_manager_ = std::make_unique<TransactionManager>(catalog_.get(), txn_timeout);
  // Restore the commit-ts high-water mark so readers see rows committed in earlier sessions. Without
  // this, the counter restarts at 0 and every persisted row (ts >= 1) is invisible to a fresh snapshot.
  txn_manager_->SetLastCommitTs(recovered_commit_ts_);
}

Database::~Database() { Close(); }

auto Database::ReadCatalogChain(std::vector<char> &record) -> bool {
  std::array<data_t, PAGE_SIZE> page{};
  dm_->ReadPage(kCatalogRootPage, page.data());
  ByteReader head(reinterpret_cast<const char *>(page.data()), PAGE_SIZE);
  if (head.GetU32() != kMagic) {
    return false;  // fresh (zero-filled) file — no catalog yet
  }
  auto total_len = head.GetU32();
  auto next = head.GetI32();

  catalog_pages_ = {kCatalogRootPage};
  record.clear();
  record.reserve(total_len);
  auto take = std::min<size_t>(kRootPayload, total_len);
  record.insert(record.end(), page.data() + 12, page.data() + 12 + take);

  // Follow the chain: collect the payload of each page until the record is assembled, then keep
  // following to record every chain page (including trailing spares) so none is leaked or reused.
  while (next != INVALID_PAGE_ID) {
    catalog_pages_.push_back(next);
    dm_->ReadPage(next, page.data());
    ByteReader body(reinterpret_cast<const char *>(page.data()), PAGE_SIZE);
    next = body.GetI32();
    if (record.size() < total_len) {
      auto chunk = std::min<size_t>(kChainPayload, total_len - record.size());
      record.insert(record.end(), page.data() + 4, page.data() + 4 + chunk);
    }
  }
  return true;
}

void Database::WriteCatalogChain(const std::vector<char> &record) {
  // How many pages the record needs; grow the chain (with permanent raw-allocated pages) to fit.
  size_t pages_needed = 1;
  if (record.size() > kRootPayload) {
    pages_needed += (record.size() - kRootPayload + kChainPayload - 1) / kChainPayload;
  }
  while (catalog_pages_.size() < pages_needed) {
    catalog_pages_.push_back(bpm_->AllocateRawPageId());
  }

  // Write each chain page: page 0 carries the magic + total length, every page carries the next
  // pointer (INVALID on the last) and its slice of the record. Trailing spare pages carry no payload.
  size_t offset = 0;
  for (size_t i = 0; i < catalog_pages_.size(); i++) {
    std::array<data_t, PAGE_SIZE> page{};
    ByteWriter header;
    if (i == 0) {
      header.PutU32(kMagic);
      header.PutU32(static_cast<uint32_t>(record.size()));
    }
    page_id_t next = (i + 1 < catalog_pages_.size()) ? catalog_pages_[i + 1] : INVALID_PAGE_ID;
    header.PutI32(next);
    std::memcpy(page.data(), header.Data().data(), header.Size());

    if (offset < record.size()) {
      size_t cap = (i == 0) ? kRootPayload : kChainPayload;
      size_t chunk = std::min(cap, record.size() - offset);
      std::memcpy(page.data() + header.Size(), record.data() + offset, chunk);
      offset += chunk;
    }
    dm_->WritePage(catalog_pages_[i], page.data());
  }
}

void Database::LoadCatalog(ByteReader &r) {
  auto version = r.GetU32();
  if (version != kVersion) {
    throw Exception("unsupported catalog format version");
  }
  auto next_page_id = r.GetI32();

  auto free_count = r.GetU32();
  std::vector<page_id_t> free_pages;
  free_pages.reserve(free_count);
  for (uint32_t i = 0; i < free_count; i++) {
    free_pages.push_back(r.GetI32());
  }
  auto next_table_oid = r.GetU32();
  auto next_index_oid = r.GetU32();
  recovered_commit_ts_ = r.GetI64();  // commit-ts high-water mark (v2), applied to the txn manager in the ctor

  // Rebuild the buffer pool seeded with the persisted allocator state, then the catalog over it.
  bpm_ = std::make_unique<BufferPoolManager>(num_frames_, dm_.get(), next_page_id);
  bpm_->SetFreePages(std::move(free_pages));
  catalog_ = std::make_unique<Catalog>(bpm_.get());

  auto table_count = r.GetU32();
  for (uint32_t t = 0; t < table_count; t++) {
    auto oid = r.GetU32();
    auto name = r.GetString();
    auto schema = DeserializeSchema(r);
    auto first_page_id = r.GetI32();
    auto last_page_id = r.GetI32();
    auto format = static_cast<StorageFormat>(r.GetU8());
    // v3: primary-key column indices, the auto-_id flag, and the _id auto-increment high-water mark.
    auto pk_count = r.GetU32();
    std::vector<uint32_t> pk_attrs;
    pk_attrs.reserve(pk_count);
    for (uint32_t k = 0; k < pk_count; k++) {
      pk_attrs.push_back(r.GetU32());
    }
    auto auto_id = r.GetU8() != 0;
    auto next_id = r.GetI64();
    catalog_->LoadTable(oid, name, schema, first_page_id, last_page_id, format, pk_attrs, auto_id, next_id);
  }

  auto index_count = r.GetU32();
  for (uint32_t i = 0; i < index_count; i++) {
    auto oid = r.GetU32();
    auto name = r.GetString();
    auto table_name = r.GetString();
    auto attr_count = r.GetU32();
    std::vector<uint32_t> key_attrs;
    key_attrs.reserve(attr_count);
    for (uint32_t a = 0; a < attr_count; a++) {
      key_attrs.push_back(r.GetU32());
    }
    auto key_size = r.GetU32();
    auto header_page_id = r.GetI32();
    auto key_schema = DeserializeSchema(r);
    LoadIndexDispatch(key_size, oid, name, table_name, key_attrs, key_schema, header_page_id);
  }

  catalog_->SetNextOids(next_table_oid, next_index_oid);
}

void Database::LoadIndexDispatch(size_t key_size, index_oid_t oid, const std::string &name,
                                 const std::string &table_name, const std::vector<uint32_t> &key_attrs,
                                 const Schema &key_schema, page_id_t header_page_id) {
  switch (key_size) {
    case 8:
      catalog_->LoadIndex<8>(oid, name, table_name, key_attrs, key_schema, header_page_id);
      break;
    case 16:
      catalog_->LoadIndex<16>(oid, name, table_name, key_attrs, key_schema, header_page_id);
      break;
    case 32:
      catalog_->LoadIndex<32>(oid, name, table_name, key_attrs, key_schema, header_page_id);
      break;
    case 64:
      catalog_->LoadIndex<64>(oid, name, table_name, key_attrs, key_schema, header_page_id);
      break;
    default:
      throw Exception("cannot reopen index '" + name + "': unsupported key size " + std::to_string(key_size));
  }
}

void Database::SerializeCatalog(ByteWriter &w) {
  w.PutU32(kVersion);
  w.PutI32(bpm_->GetNextPageId());

  auto free_pages = bpm_->GetFreePages();
  w.PutU32(static_cast<uint32_t>(free_pages.size()));
  for (auto id : free_pages) {
    w.PutI32(id);
  }
  w.PutU32(catalog_->GetNextTableOid());
  w.PutU32(catalog_->GetNextIndexOid());
  // Persist the commit-ts high-water mark so a reopened database keeps handing out timestamps above every
  // row already on disk; restored via TransactionManager::SetLastCommitTs on reopen.
  w.PutI64(txn_manager_ != nullptr ? txn_manager_->GetLastCommitTs() : 0);

  auto tables = catalog_->GetTables();
  w.PutU32(static_cast<uint32_t>(tables.size()));
  for (const auto &t : tables) {
    w.PutU32(t->oid_);
    w.PutString(t->name_);
    SerializeSchema(w, t->schema_);
    // Only row-format tables carry page ids; a metadata-only catalog would have no storage, but a
    // Database always has a buffer pool so every table here is backed by a TableHeap.
    auto *heap = static_cast<TableHeap *>(t->storage_.get());
    w.PutI32(heap->GetFirstPageId());
    w.PutI32(heap->GetLastPageId());
    w.PutU8(static_cast<uint8_t>(t->storage_->GetFormat()));
    // v3: primary-key column indices, the auto-_id flag, and the _id auto-increment high-water mark.
    w.PutU32(static_cast<uint32_t>(t->pk_attrs_.size()));
    for (auto a : t->pk_attrs_) {
      w.PutU32(a);
    }
    w.PutU8(t->auto_id_ ? 1 : 0);
    w.PutI64(t->next_id_.load());
  }

  auto indexes = catalog_->GetIndexes();
  w.PutU32(static_cast<uint32_t>(indexes.size()));
  for (const auto &idx : indexes) {
    w.PutU32(idx->oid_);
    w.PutString(idx->name_);
    w.PutString(idx->table_name_);
    const auto &key_attrs = idx->index_->GetMetadata()->GetKeyAttrs();
    w.PutU32(static_cast<uint32_t>(key_attrs.size()));
    for (auto a : key_attrs) {
      w.PutU32(a);
    }
    w.PutU32(static_cast<uint32_t>(idx->index_->GetKeySize()));
    w.PutI32(idx->index_->GetHeaderPageId());
    SerializeSchema(w, idx->key_schema_);
  }
}

void Database::Close() {
  if (closed_) {
    return;
  }
  // TODO(WAL): there is no write-ahead log, so this is clean-shutdown durability only. A crash
  // between the last Close and now can leave the catalog root / allocator high-water stale, which
  // could reissue a live page id. Crash recovery (WAL redo/undo replay) is a future milestone.

  // Size the record, then reserve any extra catalog pages the record needs. Reserving raw page ids
  // bumps only the fixed-width high-water mark (not the free list), so the record size is unchanged;
  // we serialize once more so the persisted next_page_id accounts for those pages, then write.
  ByteWriter sizing;
  SerializeCatalog(sizing);
  size_t pages_needed = 1;
  if (sizing.Size() > kRootPayload) {
    pages_needed += (sizing.Size() - kRootPayload + kChainPayload - 1) / kChainPayload;
  }
  while (catalog_pages_.size() < pages_needed) {
    catalog_pages_.push_back(bpm_->AllocateRawPageId());
  }

  // Abort every still-open transaction BEFORE flushing, so no uncommitted, temp-stamped row (whose
  // in-memory undo chain is about to vanish) reaches disk — a reopened database must see only
  // committed state. Rolled-back pages are dirtied here and picked up by the flush below.
  if (txn_manager_ != nullptr) {
    txn_manager_->AbortAllRunning();
  }

  ByteWriter record;
  SerializeCatalog(record);
  BUMBLEBEE_ASSERT(record.Size() == sizing.Size(), "catalog record size must be stable after reserving chain pages");
  WriteCatalogChain(record.Data());

  bpm_->FlushAllPages();
  dm_->ShutDown();
  closed_ = true;
}

}  // namespace bumblebee
