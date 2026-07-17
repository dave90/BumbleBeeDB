//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// table_heap.cpp
//
// Identification: src/storage/table/table_heap.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/table/table_heap.h"

#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "storage/mvcc/mvcc.h"
#include "storage/page/table_page.h"
#include "storage/row/row_operations.h"

namespace bumblebee {

namespace {

/** @brief Build a `RowLayout` from a schema's column types. */
auto LayoutFromSchema(const Schema &schema) -> RowLayout {
  std::vector<LogicalType> types;
  types.reserve(schema.GetColumnCount());
  for (const auto &col : schema.GetColumns()) {
    types.push_back(col.GetType());
  }
  RowLayout layout;
  layout.Initialize(std::move(types));
  return layout;
}

/** @brief A `Vector` of raw row pointers (physically `UBIGINT`), reused across a scan. */
auto MakePointerVector() -> Vector { return Vector{LogicalType{LogicalTypeId::UBIGINT}}; }

}  // namespace

/**
 * @brief A sequential scan over a `TableHeap`, one page (one chunk) at a time.
 *
 * The current page's guard is held across the `Next` return so gathered strings can reference the
 * pinned frame zero-copy; it is dropped when the next page is pulled or the scan is destroyed.
 */
class HeapScan : public TableScan {
 public:
  HeapScan(TableHeap *heap, std::vector<idx_t> projection, TransactionManager *txn_mgr = nullptr,
           Transaction *txn = nullptr, ScanPredicate predicate = {})
      : heap_(heap),
        projection_(std::move(projection)),
        current_page_(heap->first_page_id_),
        txn_mgr_(txn_mgr),
        txn_(txn),
        predicate_(std::move(predicate)) {
    if (projection_.empty()) {
      for (idx_t i = 0; i < heap_->layout_.GetColumnCount(); i++) {
        projection_.push_back(i);
      }
    }
    // Snapshot the end of the heap at scan-open time: the last page and its live slot count. Rows
    // appended after this point are not visited (the Halloween problem) — inserts only ever extend
    // the tail page, so earlier pages are frozen and only the stop page can grow past the bound.
    std::lock_guard lock(heap_->latch_);
    stop_page_id_ = heap_->last_page_id_;
    auto guard = heap_->bpm_->ReadPage(stop_page_id_);
    stop_slot_ = guard.As<TablePage>()->GetNumTuples();
  }

  auto Next(DataChunk &out, Vector *row_ids) -> bool override {
    const bool mvcc = txn_ != nullptr;
    while (current_page_ != INVALID_PAGE_ID) {
      auto guard = heap_->bpm_->ReadPage(current_page_);
      const auto *page = guard.As<TablePage>();
      bool at_stop_page = current_page_ == stop_page_id_;
      // On the stop page, only the rows that existed when the scan opened; elsewhere, all rows.
      auto num_slots = at_stop_page ? stop_slot_ : page->GetNumTuples();

      auto ptrs = FlatVector::GetData<data_ptr_t>(row_ptrs_);
      recon_buffers_.clear();
      recon_buffers_.reserve(num_slots);
      idx_t count = 0;
      int64_t rids[STANDARD_VECTOR_SIZE];
      for (uint16_t slot = 0; slot < num_slots; slot++) {
        auto [meta, row_ptr, size] = page->GetRow(slot);
        if (mvcc) {
          // Reconstruct the version visible to the snapshot from the base tuple we hold latched.
          // Not-visible rows (deleted-in-snapshot, created-after, or a still-uncommitted other txn)
          // are dropped. We must NOT re-latch this page, so reconstruct in place from the base.
          auto head_link = txn_mgr_->GetUndoLink(RID(current_page_, slot));
          auto visible = ReconstructVisible(txn_mgr_, txn_, meta, row_ptr, size, head_link);
          if (!visible.has_value()) {
            continue;
          }
          recon_buffers_.push_back(std::move(*visible));
          if (predicate_ && !predicate_(heap_->layout_, reinterpret_cast<const_data_ptr_t>(recon_buffers_.back().data()))) {
            recon_buffers_.pop_back();  // filtered out — reclaim the buffer we just staged
            continue;
          }
          ptrs[count] = reinterpret_cast<data_ptr_t>(recon_buffers_.back().data());
        } else {
          if (meta.is_deleted_) {
            continue;  // bug #8: scans skip logically deleted rows
          }
          if (predicate_ && !predicate_(heap_->layout_, row_ptr)) {
            continue;  // filtered out by the scan predicate
          }
          ptrs[count] = const_cast<data_ptr_t>(reinterpret_cast<const_data_ptr_t>(row_ptr));
        }
        rids[count] = RID(current_page_, slot).Get();
        count++;
      }

      // Never follow past the stop page (pages linked after the scan opened are not part of it).
      auto next = at_stop_page ? INVALID_PAGE_ID : page->GetNextPageId();
      if (count == 0) {
        current_page_ = next;
        continue;  // empty (or all-not-visible) page
      }

      // MVCC rows live in `recon_buffers_` (freed next call), so copy their strings into `out`.
      for (idx_t k = 0; k < projection_.size(); k++) {
        RowOperations::FullScanColumn(heap_->layout_, row_ptrs_, out.data_[k], count, projection_[k],
                                      /*copy_strings=*/mvcc);
      }
      out.SetCardinality(count);
      if (row_ids != nullptr) {
        auto rid_out = FlatVector::GetData<int64_t>(*row_ids);
        for (idx_t i = 0; i < count; i++) {
          rid_out[i] = rids[i];
        }
      }

      guard_ = std::move(guard);  // keep pinned so a non-MVCC chunk's zero-copy strings stay valid
      current_page_ = next;
      return true;
    }
    guard_.reset();
    return false;
  }

 private:
  TableHeap *heap_;
  std::vector<idx_t> projection_;
  page_id_t current_page_;
  page_id_t stop_page_id_{INVALID_PAGE_ID};
  uint32_t stop_slot_{0};
  std::optional<ReadPageGuard> guard_;
  Vector row_ptrs_{MakePointerVector()};
  TransactionManager *txn_mgr_{nullptr};
  Transaction *txn_{nullptr};
  // Filters rows to those it matches; empty ⇒ match every row (unfiltered scan).
  ScanPredicate predicate_;
  // Owns reconstructed older-version bytes for the current chunk (MVCC scans only).
  std::vector<std::vector<char>> recon_buffers_;
};

TableHeap::TableHeap(BufferPoolManager *bpm, SchemaRef schema)
    : bpm_(bpm), schema_(std::move(schema)), layout_(LayoutFromSchema(*schema_)) {
  first_page_id_ = bpm_->NewPage();
  last_page_id_ = first_page_id_;
  auto guard = bpm_->WritePage(first_page_id_);
  guard.AsMut<TablePage>()->Init();
}

TableHeap::TableHeap(BufferPoolManager *bpm, SchemaRef schema, page_id_t first_page_id, page_id_t last_page_id)
    : bpm_(bpm),
      schema_(std::move(schema)),
      layout_(LayoutFromSchema(*schema_)),
      first_page_id_(first_page_id),
      last_page_id_(last_page_id) {
  // Recovery path: the pages already exist on disk with their contents and next-page chain intact —
  // do NOT NewPage()/Init(), which would allocate a fresh page and wipe the table.
}

auto TableHeap::MakeScan(const std::vector<idx_t> &projection) -> std::unique_ptr<TableScan> {
  return std::make_unique<HeapScan>(this, projection);
}

auto TableHeap::MakeMvccScan(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, ScanPredicate predicate,
                             const std::vector<idx_t> &projection) -> std::unique_ptr<TableScan> {
  // An empty predicate means "match every row" — normalize it to an explicit always-true so the read
  // set recorded below is the honest whole-table predicate and the scan filter matches it exactly.
  if (!predicate) {
    predicate = [](const RowLayout & /*layout*/, const_data_ptr_t /*row*/) { return true; };
  }
  if (txn != nullptr && txn->GetIsolationLevel() == IsolationLevel::SERIALIZABLE) {
    // The scan returns exactly the rows matching `predicate`, so that predicate *is* the read set:
    // record it for commit-time phantom validation. A whole-table default records the conservative
    // "true" predicate, flagging any concurrent write to this table as a conflict.
    txn->AppendScanPredicate(oid, predicate);
  }
  return std::make_unique<HeapScan>(this, projection, txn_mgr, txn, std::move(predicate));
}

auto TableHeap::ComputeRowSizes(DataChunk &chunk) const -> std::vector<uint32_t> {
  auto count = chunk.GetSize();
  std::vector<uint32_t> sizes(count, static_cast<uint32_t>(layout_.GetFixedRowWidth()));
  if (!layout_.AllConstant()) {
    auto col_data = chunk.Orrify();
    const auto &types = layout_.GetTypes();
    for (idx_t col_no = 0; col_no < types.size(); col_no++) {
      if (types[col_no].GetPhysicalType() != PhysicalType::STRING) {
        continue;
      }
      auto &col = col_data[col_no];
      auto data = reinterpret_cast<const string_t *>(col.data_);
      for (idx_t i = 0; i < count; i++) {
        auto idx = col.sel_->GetIndex(i);
        if (col.validity_ == nullptr || col.validity_->RowIsValid(idx)) {
          sizes[i] += static_cast<uint32_t>(data[idx].Size());
        }
      }
    }
  }
  return sizes;
}

auto TableHeap::AppendRowBytes(const TupleMeta &meta, const_data_ptr_t row_data, uint16_t row_size) -> RID {
  std::lock_guard lock(latch_);
  while (true) {
    auto guard = bpm_->WritePage(last_page_id_);
    auto *page = guard.AsMut<TablePage>();
    auto slot = page->InsertRow(meta, row_data, row_size);
    if (slot.has_value()) {
      return RID(last_page_id_, *slot);
    }
    // The row did not fit. If the page is empty it never will — the row is larger than a page.
    if (page->GetNumTuples() == 0) {
      throw ExecutionException("row is too large to fit in a page");
    }
    // Otherwise link a fresh page and retry there.
    auto new_page_id = bpm_->NewPage();
    page->SetNextPageId(new_page_id);
    auto new_guard = bpm_->WritePage(new_page_id);
    new_guard.AsMut<TablePage>()->Init();
    last_page_id_ = new_page_id;
  }
}

auto TableHeap::ScatterChunk(DataChunk &chunk) -> ScatteredRows {
  auto count = chunk.GetSize();
  ScatteredRows out;
  out.sizes = ComputeRowSizes(chunk);
  out.offsets.resize(count);

  // One contiguous buffer for the whole chunk; scatter all rows in a single SIMD pass.
  size_t total = 0;
  for (idx_t i = 0; i < count; i++) {
    out.offsets[i] = total;
    total += out.sizes[i];
  }
  out.buffer.assign(total, 0);

  Vector rows = MakePointerVector();
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < count; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(out.buffer.data() + out.offsets[i]);
  }
  SelectionVector identity;
  RowOperations::Scatter(chunk, layout_, rows, identity, count);
  return out;
}

void TableHeap::Append(DataChunk &chunk, Vector &out_rids) {
  auto count = chunk.GetSize();
  if (count == 0) {
    return;
  }
  auto scattered = ScatterChunk(chunk);
  auto rid_out = FlatVector::GetData<int64_t>(out_rids);
  for (idx_t i = 0; i < count; i++) {
    auto rid = AppendRowBytes(TupleMeta{0, false}, scattered.RowAt(i), static_cast<uint16_t>(scattered.sizes[i]));
    rid_out[i] = rid.Get();
  }
}

void TableHeap::Delete(Vector &row_ids, idx_t count) {
  auto rid_data = FlatVector::GetData<int64_t>(row_ids);
  for (idx_t i = 0; i < count; i++) {
    RID rid(rid_data[i]);
    auto guard = bpm_->WritePage(rid.GetPageId());
    auto *page = guard.AsMut<TablePage>();
    auto meta = page->GetTupleMeta(rid.GetSlotNum());
    meta.is_deleted_ = true;
    page->UpdateTupleMeta(meta, rid.GetSlotNum());
  }
}

void TableHeap::Update(Vector &row_ids, DataChunk &chunk) {
  // Same-RID update: the row keeps its slot (and RID) so index entries and MVCC version chains stay
  // valid. A size-changing row is absorbed by compacting the page in place; only a row too large for
  // the page's free space is rejected (a cross-page move would break the stable RID — out of scope).
  auto count = chunk.GetSize();
  auto scattered = ScatterChunk(chunk);
  auto rid_data = FlatVector::GetData<int64_t>(row_ids);
  for (idx_t i = 0; i < count; i++) {
    RID rid(rid_data[i]);
    auto guard = bpm_->WritePage(rid.GetPageId());
    auto *page = guard.AsMut<TablePage>();
    auto [meta, old_ptr, old_size] = page->GetRow(rid.GetSlotNum());
    if (!page->UpdateRow(meta, scattered.RowAt(i), static_cast<uint16_t>(scattered.sizes[i]), rid.GetSlotNum())) {
      throw ExecutionException("updated row is too large to fit in its page");
    }
  }
}

void TableHeap::Fetch(Vector &row_ids, idx_t count, DataChunk &out) {
  auto rid_data = FlatVector::GetData<int64_t>(row_ids);

  // Copy each addressed row's bytes into a scratch buffer so `out` never references a page frame
  // (the RIDs may span many pages, which cannot all stay pinned). Strings are copied into out's heap.
  std::vector<char> scratch;
  std::vector<size_t> row_offsets(count);
  for (idx_t i = 0; i < count; i++) {
    RID rid(rid_data[i]);
    auto guard = bpm_->ReadPage(rid.GetPageId());
    const auto *page = guard.As<TablePage>();
    auto [meta, row_ptr, size] = page->GetRow(rid.GetSlotNum());
    row_offsets[i] = scratch.size();
    scratch.insert(scratch.end(), row_ptr, row_ptr + size);
  }

  Vector rows = MakePointerVector();
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  for (idx_t i = 0; i < count; i++) {
    ptrs[i] = reinterpret_cast<data_ptr_t>(scratch.data() + row_offsets[i]);
  }

  for (idx_t col_no = 0; col_no < layout_.GetColumnCount(); col_no++) {
    RowOperations::FullScanColumn(layout_, rows, out.data_[col_no], count, col_no, /*copy_strings=*/true);
  }
  out.SetCardinality(count);
}

}  // namespace bumblebee
