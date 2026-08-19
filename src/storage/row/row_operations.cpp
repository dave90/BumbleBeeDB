//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// row_operations.cpp
//
// Identification: src/storage/row/row_operations.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/row/row_operations.h"

#include <cstring>
#include <vector>

#include "common/exception.h"
#include "common/helper.h"
#include "type/bumble_string.h"
#include "type/physical_type_dispatch.h"

namespace bumblebee {

/** @brief Store a fixed-width column into every row at `col_offset`. */
template <class T>
static void TemplatedScatter(const VectorData &col, data_ptr_t *ptrs, const SelectionVector &sel, idx_t count,
                             idx_t col_offset) {
  auto data = reinterpret_cast<const T *>(col.data_);
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel.GetIndex(i);
    auto col_idx = col.sel_->GetIndex(idx);
    Store<T>(data[col_idx], ptrs[idx] + col_offset);
  }
}

/** @brief Store a variable-length column: payload after the fixed region, referenced by a handle. */
static void ScatterStrings(const VectorData &col, data_ptr_t *ptrs, const SelectionVector &sel, idx_t count,
                           idx_t col_offset, std::vector<uint32_t> &payload_cursor) {
  auto data = reinterpret_cast<const string_t *>(col.data_);
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel.GetIndex(i);
    auto col_idx = col.sel_->GetIndex(idx);
    auto row = ptrs[idx];

    bool is_valid = (col.validity_ == nullptr) || col.validity_->RowIsValid(col_idx);
    uint32_t off = payload_cursor[i];
    if (is_valid) {
      const auto &str = data[col_idx];
      auto len = static_cast<uint32_t>(str.Size());
      std::memcpy(row + off, str.GetDataUnsafe(), len);
      Store<StringHandle>(StringHandle{off, len}, row + col_offset);
      payload_cursor[i] += len;
    } else {
      // NULL: an empty payload. The validity prefix already records the null.
      Store<StringHandle>(StringHandle{off, 0}, row + col_offset);
    }
  }
}

/** @brief Load a fixed-width column out of every row into `col`, in identity order. */
template <class T>
static void TemplatedFullScan(data_ptr_t *ptrs, Vector &col, idx_t count, idx_t col_offset) {
  auto data = FlatVector::GetData<T>(col);
  for (idx_t i = 0; i < count; i++) {
    data[i] = Load<T>(ptrs[i] + col_offset);
  }
}

/** @brief Load a fixed-width column out of `count` selected rows into the selected slots of `col`. */
template <class T>
static void TemplatedGather(data_ptr_t *ptrs, const SelectionVector &row_sel, Vector &col,
                            const SelectionVector &col_sel, idx_t count, idx_t col_offset) {
  auto data = FlatVector::GetData<T>(col);
  for (idx_t i = 0; i < count; i++) {
    auto row = ptrs[row_sel.GetIndex(i)];
    if (row != nullptr) {
      data[col_sel.GetIndex(i)] = Load<T>(row + col_offset);
    }
  }
}

/**
 * @brief Filter the candidates on one fixed-width key column.
 *
 * Reads the survivors of the previous column through `cur_sel`, keeps the still-equal ones in
 * `next_sel` and appends the failures to `no_match_sel` (both as candidate positions).
 */
template <class T, bool NULL_EQUAL>
static auto TemplatedMatchCol(const VectorData &col, data_ptr_t *rows, const SelectionVector &row_sel,
                              const SelectionVector &col_sel, const SelectionVector &cur_sel, idx_t count,
                              idx_t col_offset, idx_t col_no, SelectionVector &next_sel, SelectionVector &no_match_sel,
                              idx_t &no_match_count) -> idx_t {
  auto data = reinterpret_cast<const T *>(col.data_);
  idx_t survivors = 0;
  for (idx_t p = 0; p < count; p++) {
    const idx_t cand = cur_sel.GetIndex(p);
    const auto row = rows[row_sel.GetIndex(cand)];
    const idx_t col_row = col.sel_->GetIndex(col_sel.GetIndex(cand));
    const bool col_valid = (col.validity_ == nullptr) || col.validity_->RowIsValid(col_row);
    const bool row_valid = RowIsValid(row, col_no);
    bool equal;
    if (col_valid && row_valid) {
      equal = data[col_row] == Load<T>(row + col_offset);
    } else if (NULL_EQUAL) {
      equal = !col_valid && !row_valid;  // IS NOT DISTINCT FROM: NULL == NULL
    } else {
      equal = false;  // SQL '=': a NULL key never matches
    }
    if (equal) {
      next_sel.SetIndex(survivors++, cand);
    } else {
      no_match_sel.SetIndex(no_match_count++, cand);
    }
  }
  return survivors;
}

/** @brief Filter the candidates on one variable-length (string) key column. */
template <bool NULL_EQUAL>
static auto TemplatedMatchStringCol(const VectorData &col, data_ptr_t *rows, const SelectionVector &row_sel,
                                    const SelectionVector &col_sel, const SelectionVector &cur_sel, idx_t count,
                                    idx_t col_offset, idx_t col_no, SelectionVector &next_sel,
                                    SelectionVector &no_match_sel, idx_t &no_match_count) -> idx_t {
  auto data = reinterpret_cast<const string_t *>(col.data_);
  idx_t survivors = 0;
  for (idx_t p = 0; p < count; p++) {
    const idx_t cand = cur_sel.GetIndex(p);
    const auto row = rows[row_sel.GetIndex(cand)];
    const idx_t col_row = col.sel_->GetIndex(col_sel.GetIndex(cand));
    const bool col_valid = (col.validity_ == nullptr) || col.validity_->RowIsValid(col_row);
    const bool row_valid = RowIsValid(row, col_no);
    bool equal;
    if (col_valid && row_valid) {
      const auto handle = Load<StringHandle>(row + col_offset);
      const auto &str = data[col_row];
      equal =
          handle.length_ == str.Size() && std::memcmp(row + handle.offset_, str.GetDataUnsafe(), handle.length_) == 0;
    } else if (NULL_EQUAL) {
      equal = !col_valid && !row_valid;
    } else {
      equal = false;
    }
    if (equal) {
      next_sel.SetIndex(survivors++, cand);
    } else {
      no_match_sel.SetIndex(no_match_count++, cand);
    }
  }
  return survivors;
}

/** @brief Dispatch one key column of Match on its physical type. */
template <bool NULL_EQUAL>
static auto MatchColumn(const VectorData &col, PhysicalType type, data_ptr_t *rows, const SelectionVector &row_sel,
                        const SelectionVector &col_sel, const SelectionVector &cur_sel, idx_t count, idx_t col_offset,
                        idx_t col_no, SelectionVector &next_sel, SelectionVector &no_match_sel, idx_t &no_match_count)
    -> idx_t {
  return DispatchNumericPhysicalType(
      type,
      [&]<class T>() {
        return TemplatedMatchCol<T, NULL_EQUAL>(col, rows, row_sel, col_sel, cur_sel, count, col_offset, col_no,
                                                next_sel, no_match_sel, no_match_count);
      },
      [&]() -> idx_t {
        if (type == PhysicalType::STRING) {
          return TemplatedMatchStringCol<NULL_EQUAL>(col, rows, row_sel, col_sel, cur_sel, count, col_offset, col_no,
                                                     next_sel, no_match_sel, no_match_count);
        }
        throw NotImplementedException("RowOperations::Match: unsupported physical type");
      });
}

void RowOperations::ScatterKeys(DataChunk &chunk, const std::vector<uint32_t> &src_cols,
                                const std::vector<idx_t> &dst_offsets, const std::vector<PhysicalType> &key_types,
                                data_ptr_t out, size_t key_stride, idx_t count) {
  if (count == 0) {
    return;
  }
  auto col_data = chunk.Orrify();

  // One pointer per key slot in the packed output buffer; scatter every key column through it.
  std::vector<data_ptr_t> ptrs(count);
  for (idx_t i = 0; i < count; i++) {
    ptrs[i] = out + i * key_stride;
  }
  SelectionVector identity;  // identity: GetIndex(i) == i

  for (size_t k = 0; k < src_cols.size(); k++) {
    auto &col = col_data[src_cols[k]];
    idx_t off = dst_offsets[k];
    DispatchNumericPhysicalType(
        key_types[k], [&]<class T>() { TemplatedScatter<T>(col, ptrs.data(), identity, count, off); },
        [&]() -> void {
          throw NotImplementedException("RowOperations::ScatterKeys: variable-length key columns are not supported");
        });
  }
}

void RowOperations::Scatter(DataChunk &columns, const RowLayout &layout, Vector &rows, const SelectionVector &sel,
                            idx_t count) {
  if (count == 0) {
    return;
  }

  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  auto col_data = columns.Orrify();
  const auto &offsets = layout.GetOffsets();
  const auto &types = layout.GetTypes();
  const idx_t flag_width = layout.GetFlagWidth();

  // Initialize the validity prefix to all-valid, then clear the bit of every NULL cell.
  if (flag_width != 0) {
    for (idx_t i = 0; i < count; i++) {
      RowSetAllValid(ptrs[sel.GetIndex(i)], flag_width);
    }
    for (idx_t col_no = 0; col_no < types.size(); col_no++) {
      auto &col = col_data[col_no];
      if (col.validity_ == nullptr || col.validity_->AllValid()) {
        continue;
      }
      for (idx_t i = 0; i < count; i++) {
        auto idx = sel.GetIndex(i);
        auto col_idx = col.sel_->GetIndex(idx);
        if (!col.validity_->RowIsValid(col_idx)) {
          RowSetInvalid(ptrs[idx], col_no);
        }
      }
    }
  }

  // Per-row cursor into the payload region (only needed when there are variable-length columns).
  std::vector<uint32_t> payload_cursor;
  if (!layout.AllConstant()) {
    payload_cursor.assign(count, static_cast<uint32_t>(layout.GetFixedRowWidth()));
  }

  for (idx_t col_no = 0; col_no < types.size(); col_no++) {
    auto &col = col_data[col_no];
    auto col_offset = offsets[col_no];
    const auto ptype = types[col_no].GetPhysicalType();
    DispatchNumericPhysicalType(
        ptype, [&]<class T>() { TemplatedScatter<T>(col, ptrs, sel, count, col_offset); },
        [&]() -> void {
          if (ptype == PhysicalType::STRING) {
            ScatterStrings(col, ptrs, sel, count, col_offset, payload_cursor);
            return;
          }
          throw NotImplementedException("RowOperations::Scatter: unsupported physical type");
        });
  }
}

void RowOperations::FullScanColumn(const RowLayout &layout, Vector &rows, Vector &col, idx_t count, idx_t col_no,
                                   bool copy_strings) {
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  const auto col_offset = layout.GetOffsets()[col_no];
  col.SetVectorType(VectorType::FLAT_VECTOR);
  col.Validity().Reset();

  DispatchNumericPhysicalType(
      col.GetType(), [&]<class T>() { TemplatedFullScan<T>(ptrs, col, count, col_offset); },
      [&]() -> void {
        if (col.GetType() != PhysicalType::STRING) {
          throw NotImplementedException("RowOperations::FullScanColumn: unsupported physical type");
        }
        auto out = FlatVector::GetData<string_t>(col);
        for (idx_t i = 0; i < count; i++) {
          auto row = ptrs[i];
          auto handle = Load<StringHandle>(row + col_offset);
          auto *bytes = reinterpret_cast<const char *>(row + handle.offset_);
          // Copy into col's own heap when the row bytes will not outlive this chunk (Fetch); otherwise
          // reference them in place (the scan keeps the page pinned across the pull).
          out[i] = copy_strings ? StringVector::AddString(col, bytes, handle.length_) : string_t(bytes, handle.length_);
        }
      });

  // Propagate the per-row validity prefix into the output mask.
  auto &mask = col.Validity();
  bool allocated = false;
  for (idx_t i = 0; i < count; i++) {
    if (!RowIsValid(ptrs[i], col_no)) {
      if (!allocated) {
        mask.EnsureWritable(count);
        allocated = true;
      }
      mask.SetInvalidUnsafe(i);
    }
  }
}

void RowOperations::Gather(const RowLayout &layout, Vector &rows, const SelectionVector &row_sel, Vector &col,
                           const SelectionVector &col_sel, idx_t count, idx_t col_no, bool copy_strings) {
  if (count == 0) {
    return;
  }
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  const auto col_offset = layout.GetOffsets()[col_no];
  BUMBLEBEE_ASSERT(col.GetVectorType() == VectorType::FLAT_VECTOR, "RowOperations::Gather: the output must be flat");

  DispatchNumericPhysicalType(
      col.GetType(), [&]<class T>() { TemplatedGather<T>(ptrs, row_sel, col, col_sel, count, col_offset); },
      [&]() -> void {
        if (col.GetType() != PhysicalType::STRING) {
          throw NotImplementedException("RowOperations::Gather: unsupported physical type");
        }
        auto out = FlatVector::GetData<string_t>(col);
        for (idx_t i = 0; i < count; i++) {
          auto row = ptrs[row_sel.GetIndex(i)];
          if (row == nullptr) {
            continue;  // the validity pass below emits the NULL
          }
          auto handle = Load<StringHandle>(row + col_offset);
          auto *bytes = reinterpret_cast<const char *>(row + handle.offset_);
          out[col_sel.GetIndex(i)] =
              copy_strings ? StringVector::AddString(col, bytes, handle.length_) : string_t(bytes, handle.length_);
        }
      });

  // NULL out every value whose row is null (LEFT-join padding) or whose validity bit is clear.
  auto &mask = col.Validity();
  bool allocated = false;
  for (idx_t i = 0; i < count; i++) {
    auto row = ptrs[row_sel.GetIndex(i)];
    if (row == nullptr || !RowIsValid(row, col_no)) {
      if (!allocated) {
        mask.EnsureWritable(STANDARD_VECTOR_SIZE);
        allocated = true;
      }
      mask.SetInvalidUnsafe(col_sel.GetIndex(i));
    }
  }
}

auto RowOperations::Match(DataChunk &columns, VectorData col_data[], const RowLayout &layout, idx_t key_count,
                          Vector &rows, const SelectionVector &row_sel, const SelectionVector &col_sel, idx_t count,
                          SelectionVector &match_sel, SelectionVector &no_match_sel, idx_t &no_match_count,
                          bool null_equal) -> idx_t {
  no_match_count = 0;
  if (count == 0) {
    return 0;
  }
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  const auto &offsets = layout.GetOffsets();
  const auto &types = layout.GetTypes();
  BUMBLEBEE_ASSERT(key_count <= types.size() && key_count <= columns.ColumnCount(),
                   "RowOperations::Match: the key prefix exceeds the layout");

  // Filter column by column: `cur` holds the surviving candidate positions, `next` receives the ones
  // still equal after this column. Failures accumulate in no_match_sel across all columns.
  SelectionVector cur(count);
  SelectionVector next(count);
  for (idx_t i = 0; i < count; i++) {
    cur.SetIndex(i, i);
  }
  idx_t remaining = count;
  for (idx_t c = 0; c < key_count && remaining > 0; c++) {
    remaining = null_equal ? MatchColumn<true>(col_data[c], types[c].GetPhysicalType(), ptrs, row_sel, col_sel, cur,
                                               remaining, offsets[c], c, next, no_match_sel, no_match_count)
                           : MatchColumn<false>(col_data[c], types[c].GetPhysicalType(), ptrs, row_sel, col_sel, cur,
                                                remaining, offsets[c], c, next, no_match_sel, no_match_count);
    std::swap(cur, next);
  }
  for (idx_t i = 0; i < remaining; i++) {
    match_sel.SetIndex(i, cur.GetIndex(i));
  }
  return remaining;
}

}  // namespace bumblebee
