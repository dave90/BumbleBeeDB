//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// prl_hash_table.cpp
//
// Identification: src/execution/prl_hash_table.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/prl_hash_table.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "type/bumble_string.h"

namespace bumblebee {

namespace {

auto NextPowerOfTwo(idx_t n) -> idx_t {
  idx_t p = 1;
  while (p < n) {
    p <<= 1;
  }
  return p;
}

}  // namespace

auto PRLHashTable::NonNullKeyRows(DataChunk &key_chunk, SelectionVector &sel) -> idx_t {
  // Fast path — the overwhelmingly common one, and the one a 30M-row probe pays per chunk: no key
  // column holds a NULL at all, so every row qualifies and the per-row `RowIsValid` (an
  // out-of-line switch on the encoding, once per row per column) is skipped entirely.
  bool has_null = false;
  for (idx_t c = 0; c < key_chunk.ColumnCount() && !has_null; c++) {
    VectorData vdata;
    key_chunk.data_[c].Orrify(key_chunk.GetSize(), vdata);
    has_null = !vdata.validity_->AllValid();
  }
  if (!has_null) {
    for (idx_t i = 0; i < key_chunk.GetSize(); i++) {
      sel.SetIndex(i, i);
    }
    return key_chunk.GetSize();
  }

  idx_t n = 0;
  for (idx_t i = 0; i < key_chunk.GetSize(); i++) {
    bool valid = true;
    for (idx_t c = 0; c < key_chunk.ColumnCount(); c++) {
      if (!key_chunk.data_[c].RowIsValid(i)) {
        valid = false;
        break;
      }
    }
    if (valid) {
      sel.SetIndex(n++, i);
    }
  }
  return n;
}

PRLHashTable::PRLHashTable(std::vector<LogicalType> types, idx_t key_count, bool null_equal_keys,
                           idx_t initial_capacity)
    : types_(std::move(types)), key_count_(key_count), null_equal_keys_(null_equal_keys) {
  BUMBLEBEE_ASSERT(!types_.empty() && key_count_ > 0 && key_count_ <= types_.size(),
                   "PRLHashTable: the key prefix must name at least one leading column");
  layout_.Initialize(types_);
  Resize(std::max<idx_t>(NextPowerOfTwo(initial_capacity), INITIAL_CAPACITY));
}

void PRLHashTable::Resize(idx_t new_capacity) {
  BUMBLEBEE_ASSERT((new_capacity & (new_capacity - 1)) == 0 && new_capacity > capacity_,
                   "PRLHashTable::Resize: the capacity must be a growing power of two");
  std::vector<HTEntry> next(new_capacity);  // value-initialized: every slot empty
  const hash_t mask = new_capacity - 1;
  for (const auto &entry : directory_) {
    if (entry.row_ == nullptr) {
      continue;
    }
    auto bucket = entry.hash_ & mask;
    while (next[bucket].row_ != nullptr) {
      bucket = (bucket + 1) & mask;
    }
    next[bucket] = entry;
  }
  directory_ = std::move(next);
  capacity_ = new_capacity;
  bitmask_ = mask;
}

auto PRLHashTable::AllocateRow(idx_t size) -> data_ptr_t {
  if (blocks_.empty() || block_used_ + size > block_capacity_) {
    block_capacity_ = std::max<idx_t>(BLOCK_SIZE, size);
    blocks_.push_back(std::make_unique<data_t[]>(block_capacity_));
    block_used_ = 0;
  }
  auto row = blocks_.back().get() + block_used_;
  block_used_ += size;
  return row;
}

auto PRLHashTable::ComputeRowSizes(DataChunk &chunk) const -> std::vector<uint32_t> {
  const idx_t count = chunk.GetSize();
  std::vector<uint32_t> sizes(count, static_cast<uint32_t>(layout_.GetFixedRowWidth()));
  if (layout_.AllConstant()) {
    return sizes;
  }
  for (idx_t col_no = 0; col_no < types_.size(); col_no++) {
    if (types_[col_no].GetPhysicalType() != PhysicalType::STRING) {
      continue;
    }
    VectorData col;
    chunk.data_[col_no].Orrify(count, col);
    auto data = reinterpret_cast<const string_t *>(col.data_);
    for (idx_t i = 0; i < count; i++) {
      auto idx = col.sel_->GetIndex(i);
      if (col.validity_ == nullptr || col.validity_->RowIsValid(idx)) {
        sizes[i] += static_cast<uint32_t>(data[idx].Size());
      }
    }
  }
  return sizes;
}

void PRLHashTable::FindOrCreateGroups(Vector &hashes, DataChunk &groups, Vector &addresses,
                                      SelectionVector *new_group_sel, idx_t *new_group_count) {
  const idx_t count = groups.GetSize();
  idx_t created = 0;
  if (new_group_count != nullptr) {
    *new_group_count = 0;
  }
  if (count == 0) {
    return;
  }
  BUMBLEBEE_ASSERT(groups.ColumnCount() == types_.size(),
                   "PRLHashTable::FindOrCreateGroups: the chunk must carry every layout column");
  BUMBLEBEE_ASSERT(!directory_stale_, "PRLHashTable::FindOrCreateGroups: call BuildDirectory after Merge");

  // Keep the directory under the load factor even if every row creates a group.
  while (count_ + count >= capacity_ ||
         static_cast<float>(count_ + count) / static_cast<float>(capacity_) > LOAD_FACTOR) {
    Resize(capacity_ * 2);
  }

  hashes.Normalify(count);
  auto hash_data = FlatVector::GetData<hash_t>(hashes);
  auto addr_data = FlatVector::GetData<data_ptr_t>(addresses);
  auto col_data = groups.Orrify();
  const auto sizes = ComputeRowSizes(groups);

  std::vector<idx_t> buckets(count);
  for (idx_t i = 0; i < count; i++) {
    buckets[i] = hash_data[i] & bitmask_;
  }

  SelectionVector sel;              // identity on the first round
  SelectionVector empty_sel(count);       // rows that created a group this round
  SelectionVector compare_sel(count);     // rows whose bucket hash matched — verify the keys
  SelectionVector round_no_match(count);  // rows that move to the next bucket
  SelectionVector match_sel(count);
  SelectionVector kernel_no_match(count);

  idx_t remaining = count;
  while (remaining > 0) {
    idx_t n_new = 0;
    idx_t n_compare = 0;
    idx_t n_no_match = 0;

    constexpr idx_t PREFETCH_DIST = 32;
    for (idx_t i = 0; i < remaining; i++) {
      // The directory slot is a random cache miss for large tables; look a few rows ahead.
      if (i + PREFETCH_DIST < remaining) {
        __builtin_prefetch(&directory_[buckets[sel.GetIndex(i + PREFETCH_DIST)]], 1, 0);
      }
      const idx_t idx = sel.GetIndex(i);
      auto &entry = directory_[buckets[idx]];
      if (entry.row_ == nullptr) {
        auto row = AllocateRow(sizes[idx]);
        entry.hash_ = hash_data[idx];
        entry.row_ = row;
        addr_data[idx] = row;
        row_addrs_.push_back(row);
        row_hashes_.push_back(hash_data[idx]);
        count_++;
        empty_sel.SetIndex(n_new++, idx);
        if (new_group_sel != nullptr) {
          new_group_sel->SetIndex(created, idx);
        }
        created++;
      } else if (entry.hash_ == hash_data[idx]) {
        addr_data[idx] = entry.row_;
        compare_sel.SetIndex(n_compare++, idx);
      } else {
        round_no_match.SetIndex(n_no_match++, idx);
      }
    }

    // One batched scatter materializes every group created this round.
    RowOperations::Scatter(groups, layout_, addresses, empty_sel, n_new);

    // Verify the hash-equal rows against their group's key prefix; failures probe the next bucket.
    idx_t kernel_failures = 0;
    RowOperations::Match(groups, col_data.get(), layout_, key_count_, addresses, compare_sel, compare_sel,
                         n_compare, match_sel, kernel_no_match, kernel_failures, null_equal_keys_);
    for (idx_t j = 0; j < kernel_failures; j++) {
      round_no_match.SetIndex(n_no_match++, compare_sel.GetIndex(kernel_no_match.GetIndex(j)));
    }

    for (idx_t j = 0; j < n_no_match; j++) {
      const idx_t idx = round_no_match.GetIndex(j);
      buckets[idx] = (buckets[idx] + 1) & bitmask_;
    }
    sel.Initialize(round_no_match);
    remaining = n_no_match;
  }

  if (new_group_count != nullptr) {
    *new_group_count = created;
  }
}

void PRLHashTable::Append(Vector &hashes, DataChunk &rows, const SelectionVector &sel, idx_t count) {
  if (count == 0) {
    return;
  }
  BUMBLEBEE_ASSERT(rows.ColumnCount() == types_.size(),
                   "PRLHashTable::Append: the chunk must carry every layout column");
  BUMBLEBEE_ASSERT(!directory_stale_, "PRLHashTable::Append: call BuildDirectory after Merge/AppendUnbuilt");
  while (count_ + count >= capacity_ ||
         static_cast<float>(count_ + count) / static_cast<float>(capacity_) > LOAD_FACTOR) {
    Resize(capacity_ * 2);
  }

  hashes.Normalify(rows.GetSize());
  auto hash_data = FlatVector::GetData<hash_t>(hashes);
  const auto sizes = ComputeRowSizes(rows);

  Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, rows.GetSize()};
  auto addr_data = FlatVector::GetData<data_ptr_t>(addresses);

  for (idx_t k = 0; k < count; k++) {
    const idx_t idx = sel.GetIndex(k);
    const hash_t h = hash_data[idx];
    auto bucket = h & bitmask_;
    while (directory_[bucket].row_ != nullptr) {
      bucket = (bucket + 1) & bitmask_;
    }
    auto row = AllocateRow(sizes[idx]);
    directory_[bucket] = HTEntry{h, row};
    addr_data[idx] = row;
    row_addrs_.push_back(row);
    row_hashes_.push_back(h);
    count_++;
  }
  RowOperations::Scatter(rows, layout_, addresses, sel, count);
}

void PRLHashTable::AppendUnbuilt(Vector &hashes, DataChunk &rows, const SelectionVector &sel, idx_t count) {
  if (count == 0) {
    return;
  }
  BUMBLEBEE_ASSERT(rows.ColumnCount() == types_.size(),
                   "PRLHashTable::AppendUnbuilt: the chunk must carry every layout column");
  hashes.Normalify(rows.GetSize());
  auto hash_data = FlatVector::GetData<hash_t>(hashes);
  const auto sizes = ComputeRowSizes(rows);

  Vector addresses{LogicalType{LogicalTypeId::UBIGINT}, rows.GetSize()};
  auto addr_data = FlatVector::GetData<data_ptr_t>(addresses);

  for (idx_t k = 0; k < count; k++) {
    const idx_t idx = sel.GetIndex(k);
    auto row = AllocateRow(sizes[idx]);
    addr_data[idx] = row;
    row_addrs_.push_back(row);
    row_hashes_.push_back(hash_data[idx]);
    count_++;
  }
  RowOperations::Scatter(rows, layout_, addresses, sel, count);
  directory_stale_ = true;
}

void PRLHashTable::Merge(PRLHashTable &other) {
  BUMBLEBEE_ASSERT(types_ == other.types_ && key_count_ == other.key_count_,
                   "PRLHashTable::Merge: the tables must share one layout");
  if (other.count_ == 0) {
    return;
  }
  // Steal the blocks and the (address, hash) pairs: rows never move, so the addresses stay valid.
  for (auto &block : other.blocks_) {
    blocks_.push_back(std::move(block));
  }
  row_addrs_.insert(row_addrs_.end(), other.row_addrs_.begin(), other.row_addrs_.end());
  row_hashes_.insert(row_hashes_.end(), other.row_hashes_.begin(), other.row_hashes_.end());
  count_ += other.count_;
  directory_stale_ = true;
  // The bump cursor described OUR old last block, but the last block is now one of `other`'s (with
  // its own fill level) — force the next AllocateRow to open a fresh block instead of clobbering it.
  block_used_ = 0;
  block_capacity_ = 0;

  other.blocks_.clear();
  other.row_addrs_.clear();
  other.row_hashes_.clear();
  other.count_ = 0;
  other.block_used_ = 0;
  other.block_capacity_ = 0;
}

void PRLHashTable::BuildDirectory() {
  // Size once from the final count (load <= 0.5), then link every stored (hash, address) pair —
  // the key bytes are never read.
  idx_t capacity = std::max<idx_t>(INITIAL_CAPACITY, NextPowerOfTwo(count_ * 2));
  directory_.assign(capacity, HTEntry{});
  capacity_ = capacity;
  bitmask_ = capacity - 1;
  for (idx_t i = 0; i < count_; i++) {
    auto bucket = row_hashes_[i] & bitmask_;
    while (directory_[bucket].row_ != nullptr) {
      bucket = (bucket + 1) & bitmask_;
    }
    directory_[bucket] = HTEntry{row_hashes_[i], row_addrs_[i]};
  }
  directory_stale_ = false;
}

PRLHashTable::ProbeState::ProbeState()
    : cand_addr_(STANDARD_VECTOR_SIZE),
      cand_row_(STANDARD_VECTOR_SIZE),
      cand_rows_vec_(LogicalType{LogicalTypeId::UBIGINT}, reinterpret_cast<data_ptr_t>(cand_addr_.data())),
      cand_col_sel_(cand_row_.data()),
      match_sel_(STANDARD_VECTOR_SIZE),
      no_match_sel_(STANDARD_VECTOR_SIZE) {}

void PRLHashTable::Probe(Vector &hashes, DataChunk &keys, const SelectionVector &sel, idx_t count,
                         std::vector<data_ptr_t> &out_addrs, std::vector<sel_t> &out_rows,
                         std::vector<uint8_t> *matched) {
  ProbeState state;
  Probe(state, hashes, keys, sel, count, out_addrs, out_rows, matched);
}

void PRLHashTable::Probe(ProbeState &state, Vector &hashes, DataChunk &keys, const SelectionVector &sel,
                         idx_t count, std::vector<data_ptr_t> &out_addrs, std::vector<sel_t> &out_rows,
                         std::vector<uint8_t> *matched) {
  if (count == 0 || count_ == 0) {
    return;
  }
  BUMBLEBEE_ASSERT(!directory_stale_, "PRLHashTable::Probe: call BuildDirectory after Merge/AppendUnbuilt");
  hashes.Normalify(keys.GetSize());
  auto hash_data = FlatVector::GetData<hash_t>(hashes);
  auto col_data = keys.Orrify();

  // Candidate batch: hash-equal directory entries awaiting key verification.
  auto &cand_addr = state.cand_addr_;
  auto &cand_row = state.cand_row_;
  idx_t n_cand = 0;

  auto flush = [&]() {
    if (n_cand == 0) {
      return;
    }
    idx_t failures = 0;
    const idx_t n = RowOperations::Match(keys, col_data.get(), layout_, key_count_, state.cand_rows_vec_,
                                         state.identity_, state.cand_col_sel_, n_cand, state.match_sel_,
                                         state.no_match_sel_, failures, null_equal_keys_);
    for (idx_t j = 0; j < n; j++) {
      const idx_t pos = state.match_sel_.GetIndex(j);
      out_addrs.push_back(cand_addr[pos]);
      out_rows.push_back(cand_row[pos]);
      if (matched != nullptr) {
        (*matched)[cand_row[pos]] = 1;
      }
    }
    n_cand = 0;
  };

  // Two passes over a small block of probe rows: the first computes each row's bucket and issues a
  // prefetch, the second chases the clusters. Bucket addresses are essentially random, so a
  // directory that outgrows the cache would otherwise stall on every single row; splitting the
  // passes lets the loads of a whole block be in flight at once. `dir`/`mask` are hoisted because
  // nothing in the loop (flush included) can touch the directory.
  static constexpr idx_t PREFETCH_BLOCK = 32;
  const auto *dir = directory_.data();
  const hash_t mask = bitmask_;
  const sel_t *sel_data = sel.GetData();
  std::array<hash_t, PREFETCH_BLOCK> block_hash{};
  std::array<idx_t, PREFETCH_BLOCK> block_bucket{};
  std::array<sel_t, PREFETCH_BLOCK> block_row{};

  for (idx_t base = 0; base < count; base += PREFETCH_BLOCK) {
    const idx_t n_block = std::min<idx_t>(PREFETCH_BLOCK, count - base);
    for (idx_t j = 0; j < n_block; j++) {
      const idx_t idx = sel_data != nullptr ? sel_data[base + j] : base + j;
      const hash_t h = hash_data[idx];
      block_row[j] = static_cast<sel_t>(idx);
      block_hash[j] = h;
      block_bucket[j] = h & mask;
      __builtin_prefetch(dir + block_bucket[j]);
    }
    for (idx_t j = 0; j < n_block; j++) {
      const hash_t h = block_hash[j];
      idx_t bucket = block_bucket[j];
      while (dir[bucket].row_ != nullptr) {
        if (dir[bucket].hash_ == h) {
          cand_addr[n_cand] = dir[bucket].row_;
          cand_row[n_cand] = block_row[j];
          if (++n_cand == STANDARD_VECTOR_SIZE) {
            flush();
          }
        }
        bucket = (bucket + 1) & mask;
      }
    }
  }
  flush();
}

auto PRLHashTable::Scan(idx_t offset, DataChunk &result, bool copy_strings) -> idx_t {
  if (offset >= count_) {
    result.SetCardinality(0);
    return 0;
  }
  const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, count_ - offset);
  // row_addrs_ is contiguous, so the scan window doubles as the gather's pointer vector.
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}, reinterpret_cast<data_ptr_t>(row_addrs_.data() + offset)};
  for (idx_t c = 0; c < result.ColumnCount(); c++) {
    RowOperations::FullScanColumn(layout_, rows, result.data_[c], n, c, copy_strings);
  }
  result.SetCardinality(n);
  return n;
}

}  // namespace bumblebee
