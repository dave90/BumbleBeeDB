//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// top_n_heap.cpp
//
// Identification: src/execution/sort/top_n_heap.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/sort/top_n_heap.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace bumblebee {

namespace {

/** @brief The prefilter row loop, monomorphized per first-key type: one flat data pointer and
 * inline validity reads instead of a per-row encoding dispatch (this loop sees every input row
 * once the heap is full, so it is the TopN sink's hot path). Same order-code construction as
 * TopNHeap::OrderCodeAt. */
template <class T, bool IS_SIGNED>
void PrefilterCodes(const T *data, const ValidityMask &validity, uint64_t threshold, bool desc, idx_t count,
                    SelectionVector &sel, idx_t &nc) {
  static constexpr uint64_t SIGN = 0x8000000000000000ULL;
  for (idx_t i = 0; i < count; i++) {
    uint64_t code;
    if constexpr (IS_SIGNED) {
      code = static_cast<uint64_t>(static_cast<int64_t>(data[i])) ^ SIGN;
    } else {
      code = static_cast<uint64_t>(data[i]);
    }
    if (desc) {
      code = ~code;
    }
    if (!validity.RowIsValid(i) || code <= threshold) {
      sel.SetIndex(nc++, i);
    }
  }
}

}  // namespace

TopNHeap::TopNHeap(const std::vector<LogicalType> &payload_types, const std::vector<LogicalType> &key_types,
                   const std::vector<OrderModifiers> &modifiers, idx_t limit)
    : modifiers_(modifiers), limit_(limit), append_sel_(MaxValue<idx_t>(STANDARD_VECTOR_SIZE, limit)),
      cand_sel_(STANDARD_VECTOR_SIZE) {
  payload_.Initialize(payload_types);
  heap_.reserve(MinValue<idx_t>(limit_, STANDARD_VECTOR_SIZE));

  // Enable the first-column prefilter only for integer first sort columns, where a
  // monotonic order-code can be derived cheaply (covers counts, dates/timestamps and
  // integer keys). Other types always take the full path.
  if (!key_types.empty()) {
    first_type_ = key_types[0].GetPhysicalType();
    first_desc_ = modifiers_[0].order_type_ == OrderType::DESCENDING;
    switch (first_type_) {
      case PhysicalType::TINYINT:
      case PhysicalType::SMALLINT:
      case PhysicalType::INTEGER:
      case PhysicalType::BIGINT:
      case PhysicalType::UTINYINT:
      case PhysicalType::USMALLINT:
      case PhysicalType::UINTEGER:
      case PhysicalType::UBIGINT:
        prefilter_enabled_ = true;
        break;
      default:
        prefilter_enabled_ = false;
    }
  }
}

auto TopNHeap::OrderCodeAt(Vector &v, idx_t i) const -> uint64_t {
  // Signed types are offset by the sign bit so the unsigned compare matches the signed
  // order; DESC inverts the code.
  static constexpr uint64_t SIGN = 0x8000000000000000ULL;
  uint64_t code;
  switch (first_type_) {
    case PhysicalType::TINYINT:
      code = static_cast<uint64_t>(static_cast<int64_t>(FlatVector::GetData<int8_t>(v)[i])) ^ SIGN;
      break;
    case PhysicalType::SMALLINT:
      code = static_cast<uint64_t>(static_cast<int64_t>(FlatVector::GetData<int16_t>(v)[i])) ^ SIGN;
      break;
    case PhysicalType::INTEGER:
      code = static_cast<uint64_t>(static_cast<int64_t>(FlatVector::GetData<int32_t>(v)[i])) ^ SIGN;
      break;
    case PhysicalType::BIGINT:
      code = static_cast<uint64_t>(FlatVector::GetData<int64_t>(v)[i]) ^ SIGN;
      break;
    case PhysicalType::UTINYINT:
      code = FlatVector::GetData<uint8_t>(v)[i];
      break;
    case PhysicalType::USMALLINT:
      code = FlatVector::GetData<uint16_t>(v)[i];
      break;
    case PhysicalType::UINTEGER:
      code = FlatVector::GetData<uint32_t>(v)[i];
      break;
    case PhysicalType::UBIGINT:
      code = FlatVector::GetData<uint64_t>(v)[i];
      break;
    default:
      return 0;  // unreachable: only called when prefilter_enabled_
  }
  return first_desc_ ? ~code : code;
}

void TopNHeap::Sink(DataChunk &input, DataChunk &keys) {
  const idx_t count = input.GetSize();
  if (limit_ == 0 || count == 0) {
    return;
  }
  // Only the first key column needs flattening — the prefilter and the entry order-codes read it
  // through FlatVector. The remaining key columns go through CreateSortKey, which Orrifies any
  // encoding itself; materializing them here (a dictionary STRING column from a filtered scan,
  // say) would copy every row just to throw most of them away at the prefilter.
  if (prefilter_enabled_) {
    keys.data_[0].Normalify(count);
  }

  // Fast path: once the heap is full, only rows whose first-column order-code is <= the
  // threshold (the worst kept entry) can still qualify — a strictly larger code sorts
  // strictly after the threshold on the dominant column — so their sort keys are never
  // built. NULL rows cannot be ruled out cheaply and stay candidates.
  if (prefilter_enabled_ && heap_.size() >= limit_ && heap_.front().first_valid_) {
    const uint64_t threshold = heap_.front().first_code_;
    Vector &first = keys.data_[0];  // flat: the chunk was just Normalified
    const auto &validity = FlatVector::Validity(first);
    idx_t nc = 0;
    switch (first_type_) {
      case PhysicalType::TINYINT:
        PrefilterCodes<int8_t, true>(FlatVector::GetData<int8_t>(first), validity, threshold, first_desc_, count,
                                     cand_sel_, nc);
        break;
      case PhysicalType::SMALLINT:
        PrefilterCodes<int16_t, true>(FlatVector::GetData<int16_t>(first), validity, threshold, first_desc_, count,
                                      cand_sel_, nc);
        break;
      case PhysicalType::INTEGER:
        PrefilterCodes<int32_t, true>(FlatVector::GetData<int32_t>(first), validity, threshold, first_desc_, count,
                                      cand_sel_, nc);
        break;
      case PhysicalType::BIGINT:
        PrefilterCodes<int64_t, true>(FlatVector::GetData<int64_t>(first), validity, threshold, first_desc_, count,
                                      cand_sel_, nc);
        break;
      case PhysicalType::UTINYINT:
        PrefilterCodes<uint8_t, false>(FlatVector::GetData<uint8_t>(first), validity, threshold, first_desc_, count,
                                       cand_sel_, nc);
        break;
      case PhysicalType::USMALLINT:
        PrefilterCodes<uint16_t, false>(FlatVector::GetData<uint16_t>(first), validity, threshold, first_desc_, count,
                                        cand_sel_, nc);
        break;
      case PhysicalType::UINTEGER:
        PrefilterCodes<uint32_t, false>(FlatVector::GetData<uint32_t>(first), validity, threshold, first_desc_, count,
                                        cand_sel_, nc);
        break;
      case PhysicalType::UBIGINT:
        PrefilterCodes<uint64_t, false>(FlatVector::GetData<uint64_t>(first), validity, threshold, first_desc_, count,
                                        cand_sel_, nc);
        break;
      default:
        // Unreachable (the prefilter only enables for the integer types above); keep the generic
        // per-row path as a safety net.
        for (idx_t i = 0; i < count; i++) {
          if (!first.RowIsValid(i) || OrderCodeAt(first, i) <= threshold) {
            cand_sel_.SetIndex(nc++, i);
          }
        }
    }
    if (nc == 0) {
      return;  // the whole chunk is pruned
    }
    if (nc < count) {
      DataChunk cand_keys;
      cand_keys.InitAndReference(keys);
      cand_keys.Slice(cand_sel_, nc);
      cand_keys.Normalify();
      SinkRows(input, cand_keys, &cand_sel_, nc);
      return;
    }
    // nc == count: every row is a candidate, fall through to the full path.
  }

  SinkRows(input, keys, nullptr, count);
}

void TopNHeap::SinkRows(DataChunk &input, DataChunk &keys, const SelectionVector *sel, idx_t count) {
  Vector sort_keys{LogicalType{LogicalTypeId::STRING}};
  CreateSortKey::Create(keys, modifiers_, sort_keys);
  const auto *key_data = FlatVector::GetData<string_t>(sort_keys);
  Vector *first = prefilter_enabled_ ? &keys.data_[0] : nullptr;

  idx_t added = 0;
  idx_t row = payload_.GetSize();
  for (idx_t i = 0; i < count; i++) {
    if (!ShouldAdd(key_data[i])) {
      continue;
    }
    TopNEntry entry{key_heap_.AddString(key_data[i]), row++};
    if (first != nullptr) {
      entry.first_valid_ = first->RowIsValid(i);
      if (entry.first_valid_) {
        entry.first_code_ = OrderCodeAt(*first, i);
      }
    }
    append_sel_.SetIndex(added++, sel != nullptr ? sel->GetIndex(i) : i);
    Push(entry);
  }

  if (added == 0) {
    return;
  }
  // Copy only the rows that entered the heap, all columns in one selection append.
  payload_.Append(input, true, &append_sel_, added);
  Reduce(false);
}

void TopNHeap::Combine(TopNHeap &other) {
  if (limit_ == 0 || other.heap_.empty()) {
    return;
  }
  // Finalizing `other` sorts its entries best-first, so the merge stops at the first
  // entry that no longer qualifies.
  other.Finalize();

  idx_t added = 0;
  idx_t row = payload_.GetSize();
  for (const auto &entry : other.heap_) {
    if (!ShouldAdd(entry.key_)) {
      break;
    }
    append_sel_.SetIndex(added++, entry.index_);
    Push(TopNEntry{key_heap_.AddString(entry.key_), row++, entry.first_code_, entry.first_valid_});
  }

  if (added == 0) {
    return;
  }
  payload_.Append(other.payload_, true, &append_sel_, added);
  Reduce(false);
}

void TopNHeap::Reduce(bool force) {
  if (!force && payload_.GetSize() < MaxValue<idx_t>(2 * STANDARD_VECTOR_SIZE, 2 * limit_)) {
    return;
  }
  if (payload_.GetSize() == heap_.size()) {
    return;  // already compact
  }

  // Keep only the live rows, in heap order, and re-index the entries to match.
  SelectionVector sel(MaxValue<idx_t>(heap_.size(), 1));
  idx_t count = 0;
  for (auto &entry : heap_) {
    sel.SetIndex(count, entry.index_);
    entry.index_ = count++;
  }
  payload_.Slice(sel, count);
  payload_.Normalify();  // materializes into fresh storage; the dead rows are freed
  payload_.SetCapacity(MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));

  // The same for the key bytes: re-add the live keys into a fresh heap.
  StringHeap compacted;
  for (auto &entry : heap_) {
    entry.key_ = compacted.AddString(entry.key_);
  }
  key_heap_ = std::move(compacted);
}

void TopNHeap::Finalize() {
  if (finalized_) {
    return;
  }
  std::sort(heap_.begin(), heap_.end());  // best first
  Reduce(true);
  finalized_ = true;
}

auto TopNHeap::GetData(DataChunk &output, idx_t pos) -> idx_t {
  BUMBLEBEE_ASSERT(finalized_, "TopNHeap::GetData before Finalize");
  if (pos >= heap_.size()) {
    return 0;
  }
  const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, heap_.size() - pos);
  SelectionVector sel(count);
  for (idx_t i = 0; i < count; i++) {
    sel.SetIndex(i, heap_[pos + i].index_);
  }
  output.Reference(payload_);
  output.Slice(sel, count);
  return count;
}

}  // namespace bumblebee
