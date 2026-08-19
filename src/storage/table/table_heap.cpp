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

/** @brief Build a `RowLayout` from a schema's column types. */
static auto LayoutFromSchema(const Schema &schema) -> RowLayout {
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
static auto MakePointerVector() -> Vector { return Vector{LogicalType{LogicalTypeId::UBIGINT}}; }

/**
 * @brief A scan over one `[begin, end)` page-index morsel of a `ParallelScanState`, a page per chunk.
 *
 * The scan addresses pages through the state's snapshotted page directory rather than by following the
 * on-disk chain, so many `HeapScan`s over disjoint morsels of the same snapshot run concurrently. The
 * current page's guard is held across the `Next` return so gathered strings can reference the pinned
 * frame zero-copy; it is dropped when the next page is pulled or the scan is destroyed.
 */
class HeapScan : public TableScan {
 public:
  HeapScan(std::shared_ptr<ParallelScanState> state, idx_t begin, idx_t end)
      : state_(std::move(state)),
        page_idx_(begin),
        page_end_(std::min<idx_t>(end, state_ ? state_->pages_.size() : 0)) {
    projection_ = state_->projection_;
    if (projection_.empty()) {
      for (idx_t i = 0; i < state_->heap_->layout_.GetColumnCount(); i++) {
        projection_.push_back(i);
      }
    }
  }

  auto Next(DataChunk &out, Vector *row_ids) -> bool override {
    auto &heap = *state_->heap_;
    auto *txn_mgr = state_->txn_mgr_;
    auto *txn = state_->txn_;
    const bool mvcc = txn != nullptr;
    const auto &predicate = state_->predicate_;
    const idx_t last_page_idx = state_->pages_.empty() ? 0 : state_->pages_.size() - 1;

    while (true) {
      // Retire a fully-drained page. The guard was held across the previous return so that page's
      // zero-copy strings stayed valid until the consumer processed the chunk; it is safe to drop now.
      if (guard_.has_value() && slot_cursor_ >= cur_num_slots_) {
        guard_.reset();
        page_idx_++;
      }
      // Pin the next page. A page holds many rows — potentially more than STANDARD_VECTOR_SIZE — so a
      // single page is drained across several `Next` calls, a vector's worth of rows at a time.
      if (!guard_.has_value()) {
        if (page_idx_ >= page_end_) {
          return false;
        }
        guard_ = heap.bpm_->ReadPage(state_->pages_[page_idx_]);
        slot_cursor_ = 0;
        // Only the last page of the snapshot carries the Halloween slot cap; earlier pages are frozen
        // (a page stops growing the moment a fresh page is linked after it), so all their slots count.
        cur_num_slots_ = page_idx_ == last_page_idx ? state_->stop_slot_ : guard_->As<TablePage>()->GetNumTuples();
      }

      const page_id_t current_page = state_->pages_[page_idx_];
      const auto *page = guard_->As<TablePage>();
      auto ptrs = FlatVector::GetData<data_ptr_t>(row_ptrs_);
      recon_buffers_.clear();
      recon_buffers_.reserve(std::min<idx_t>(STANDARD_VECTOR_SIZE, cur_num_slots_));
      idx_t count = 0;
      int64_t rids[STANDARD_VECTOR_SIZE];
      // Gather up to a full vector of visible rows, resuming where the previous chunk left off.
      for (; slot_cursor_ < cur_num_slots_ && count < STANDARD_VECTOR_SIZE; slot_cursor_++) {
        const uint16_t slot = slot_cursor_;
        auto [meta, row_ptr, size] = page->GetRow(slot);
        if (mvcc) {
          // Reconstruct the version visible to the snapshot from the base tuple we hold latched.
          // Not-visible rows (deleted-in-snapshot, created-after, or a still-uncommitted other txn)
          // are dropped. We must NOT re-latch this page, so reconstruct in place from the base.
          auto head_link = txn_mgr->GetUndoLink(RID(current_page, slot));
          auto visible = ReconstructVisible(txn_mgr, txn, meta, row_ptr, size, head_link);
          if (!visible.has_value()) {
            continue;
          }
          recon_buffers_.push_back(std::move(*visible));
          if (predicate && !predicate(heap.layout_, reinterpret_cast<const_data_ptr_t>(recon_buffers_.back().data()))) {
            recon_buffers_.pop_back();  // filtered out — reclaim the buffer we just staged
            continue;
          }
          ptrs[count] = reinterpret_cast<data_ptr_t>(recon_buffers_.back().data());
        } else {
          if (meta.is_deleted_) {
            continue;  // scans skip logically deleted rows
          }
          if (predicate && !predicate(heap.layout_, row_ptr)) {
            continue;  // filtered out by the scan predicate
          }
          ptrs[count] = const_cast<data_ptr_t>(reinterpret_cast<const_data_ptr_t>(row_ptr));
        }
        rids[count] = RID(current_page, slot).Get();
        count++;
      }

      if (count == 0) {
        // Every remaining slot on this page was filtered / not visible; move to the next page.
        guard_.reset();
        page_idx_++;
        continue;
      }

      // MVCC rows live in `recon_buffers_` (freed next call), so copy their strings into `out`.
      for (idx_t k = 0; k < projection_.size(); k++) {
        RowOperations::FullScanColumn(heap.layout_, row_ptrs_, out.data_[k], count, projection_[k],
                                      /*copy_strings=*/mvcc);
      }
      out.SetCardinality(count);
      if (row_ids != nullptr) {
        auto rid_out = FlatVector::GetData<int64_t>(*row_ids);
        for (idx_t i = 0; i < count; i++) {
          rid_out[i] = rids[i];
        }
      }

      // Keep `guard_` pinned across the return so a non-MVCC chunk's zero-copy strings stay valid; the
      // page is retired at the top of the next call once `slot_cursor_` has reached `cur_num_slots_`.
      return true;
    }
  }

 private:
  std::shared_ptr<ParallelScanState> state_;
  idx_t page_idx_;
  idx_t page_end_;
  std::vector<idx_t> projection_;
  std::optional<ReadPageGuard> guard_;
  /** The next slot to read on the currently pinned page — a page is drained a vector at a time. */
  uint16_t slot_cursor_{0};
  /** The live-slot count of the currently pinned page (cached so the page is not re-read to retire it). */
  idx_t cur_num_slots_{0};
  Vector row_ptrs_{MakePointerVector()};
  // Owns reconstructed older-version bytes for the current chunk (MVCC scans only).
  std::vector<std::vector<char>> recon_buffers_;
};

auto ParallelScanState::NextMorsel(idx_t &begin, idx_t &end) -> bool {
  const idx_t start = next_page_idx_.fetch_add(morsel_pages_, std::memory_order_relaxed);
  if (start >= pages_.size()) {
    return false;
  }
  begin = start;
  end = std::min<idx_t>(start + morsel_pages_, pages_.size());
  return true;
}

TableHeap::TableHeap(BufferPoolManager *bpm, SchemaRef schema)
    : bpm_(bpm), schema_(std::move(schema)), layout_(LayoutFromSchema(*schema_)) {
  first_page_id_ = bpm_->NewPage();
  last_page_id_ = first_page_id_;
  auto guard = bpm_->WritePage(first_page_id_);
  guard.AsMut<TablePage>()->Init();
  page_directory_.push_back(first_page_id_);
}

TableHeap::TableHeap(BufferPoolManager *bpm, SchemaRef schema, page_id_t first_page_id, page_id_t last_page_id)
    : bpm_(bpm),
      schema_(std::move(schema)),
      layout_(LayoutFromSchema(*schema_)),
      first_page_id_(first_page_id),
      last_page_id_(last_page_id) {
  // Recovery path: the pages already exist on disk with their contents and next-page chain intact —
  // do NOT NewPage()/Init(), which would allocate a fresh page and wipe the table. Rebuild the page
  // directory by walking the on-disk chain once (the chain is the source of truth on open).
  for (page_id_t pid = first_page_id_; pid != INVALID_PAGE_ID;) {
    page_directory_.push_back(pid);
    auto guard = bpm_->ReadPage(pid);
    pid = guard.As<TablePage>()->GetNextPageId();
  }
}

auto TableHeap::BeginParallelScan(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid,
                                  ScanPredicate predicate, const std::vector<idx_t> &projection)
    -> std::shared_ptr<ParallelScanState> {
  // Under MVCC an empty predicate means "match every row" — normalize it to an explicit always-true so
  // the read set recorded below is the honest whole-table predicate and the scan filter matches it.
  if (txn != nullptr && !predicate) {
    predicate = [](const RowLayout & /*layout*/, const_data_ptr_t /*row*/) { return true; };
  }
  auto state = std::make_shared<ParallelScanState>();
  state->heap_ = this;
  state->txn_mgr_ = txn_mgr;
  state->txn_ = txn;
  state->projection_ = projection;
  state->predicate_ = predicate;
  {
    // Snapshot the page directory + the tail page's live-slot count atomically: rows appended after
    // this point (the Halloween problem) are not part of the scan. Same lock order as AppendRowBytes
    // (latch_ then a page guard), so the two never deadlock.
    std::lock_guard lock(latch_);
    state->pages_ = page_directory_;
    auto guard = bpm_->ReadPage(last_page_id_);
    state->stop_slot_ = guard.As<TablePage>()->GetNumTuples();
  }
  if (txn != nullptr && txn->GetIsolationLevel() == IsolationLevel::SERIALIZABLE) {
    // The scan returns exactly the rows matching `predicate`, so that predicate *is* the read set:
    // record it once for commit-time phantom validation (once per scan, not once per morsel).
    txn->AppendScanPredicate(oid, predicate);
  }
  return state;
}

auto TableHeap::MakeMorselScan(const std::shared_ptr<ParallelScanState> &state, idx_t begin, idx_t end)
    -> std::unique_ptr<TableScan> {
  return std::make_unique<HeapScan>(state, begin, end);
}

auto TableHeap::MakeScan(const std::vector<idx_t> &projection) -> std::unique_ptr<TableScan> {
  auto state = BeginParallelScan(nullptr, nullptr, 0, {}, projection);
  return MakeMorselScan(state, 0, state->NumPages());
}

auto TableHeap::EstimatedRowCount() const -> idx_t {
  // Snapshot the page list under the latch, then sum each page's slot count. This over-counts
  // logically-deleted rows (an estimate is all the planner needs) and is O(pages) — infrequent
  // (statistics / planning time), never on the row path.
  std::vector<page_id_t> pages;
  {
    std::lock_guard lock(latch_);
    pages = page_directory_;
  }
  idx_t total = 0;
  for (const auto pid : pages) {
    total += bpm_->ReadPage(pid).As<TablePage>()->GetNumTuples();
  }
  return total;
}

auto TableHeap::MakeMvccScan(TransactionManager *txn_mgr, Transaction *txn, table_oid_t oid, ScanPredicate predicate,
                             const std::vector<idx_t> &projection) -> std::unique_ptr<TableScan> {
  auto state = BeginParallelScan(txn_mgr, txn, oid, std::move(predicate), projection);
  return MakeMorselScan(state, 0, state->NumPages());
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
    page_directory_.push_back(new_page_id);  // extend the parallel-scan substrate under latch_
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

void TableHeap::FreeAllPages() {
  std::lock_guard lk(latch_);
  // `page_directory_` is the complete list of pages this heap ever linked, so it is enough to walk it
  // rather than re-traverse the on-disk chain. DeletePage returns each id to the persistent free list.
  for (page_id_t pid : page_directory_) {
    bpm_->DeletePage(pid);
  }
  page_directory_.clear();
  first_page_id_ = INVALID_PAGE_ID;
  last_page_id_ = INVALID_PAGE_ID;
}

}  // namespace bumblebee
