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

namespace bumblebee {

namespace {

/** @brief Store a fixed-width column into every row at `col_offset`. */
template <class T>
void TemplatedScatter(const VectorData &col, data_ptr_t *ptrs, const SelectionVector &sel, idx_t count,
                      idx_t col_offset) {
  auto data = reinterpret_cast<const T *>(col.data_);
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel.GetIndex(i);
    auto col_idx = col.sel_->GetIndex(idx);
    Store<T>(data[col_idx], ptrs[idx] + col_offset);
  }
}

/** @brief Store a variable-length column: payload after the fixed region, referenced by a handle. */
void ScatterStrings(const VectorData &col, data_ptr_t *ptrs, const SelectionVector &sel, idx_t count,
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
void TemplatedFullScan(data_ptr_t *ptrs, Vector &col, idx_t count, idx_t col_offset) {
  auto data = FlatVector::GetData<T>(col);
  for (idx_t i = 0; i < count; i++) {
    data[i] = Load<T>(ptrs[i] + col_offset);
  }
}

}  // namespace

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
    switch (key_types[k]) {
      case PhysicalType::TINYINT:
        TemplatedScatter<int8_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::SMALLINT:
        TemplatedScatter<int16_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::INTEGER:
        TemplatedScatter<int32_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::BIGINT:
        TemplatedScatter<int64_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::UTINYINT:
        TemplatedScatter<uint8_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::USMALLINT:
        TemplatedScatter<uint16_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::UINTEGER:
        TemplatedScatter<uint32_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::UBIGINT:
        TemplatedScatter<uint64_t>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::FLOAT:
        TemplatedScatter<float>(col, ptrs.data(), identity, count, off);
        break;
      case PhysicalType::DOUBLE:
        TemplatedScatter<double>(col, ptrs.data(), identity, count, off);
        break;
      default:
        throw NotImplementedException("RowOperations::ScatterKeys: variable-length key columns are not supported");
    }
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
    switch (types[col_no].GetPhysicalType()) {
      case PhysicalType::TINYINT:
        TemplatedScatter<int8_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::SMALLINT:
        TemplatedScatter<int16_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::INTEGER:
        TemplatedScatter<int32_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::BIGINT:
        TemplatedScatter<int64_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::UTINYINT:
        TemplatedScatter<uint8_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::USMALLINT:
        TemplatedScatter<uint16_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::UINTEGER:
        TemplatedScatter<uint32_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::UBIGINT:
        TemplatedScatter<uint64_t>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::FLOAT:
        TemplatedScatter<float>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::DOUBLE:
        TemplatedScatter<double>(col, ptrs, sel, count, col_offset);
        break;
      case PhysicalType::STRING:
        ScatterStrings(col, ptrs, sel, count, col_offset, payload_cursor);
        break;
      default:
        throw NotImplementedException("RowOperations::Scatter: unsupported physical type");
    }
  }
}

void RowOperations::FullScanColumn(const RowLayout &layout, Vector &rows, Vector &col, idx_t count, idx_t col_no,
                                   bool copy_strings) {
  auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
  const auto col_offset = layout.GetOffsets()[col_no];
  col.SetVectorType(VectorType::FLAT_VECTOR);
  col.Validity().Reset();

  switch (col.GetType()) {
    case PhysicalType::TINYINT:
      TemplatedFullScan<int8_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::SMALLINT:
      TemplatedFullScan<int16_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::INTEGER:
      TemplatedFullScan<int32_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::BIGINT:
      TemplatedFullScan<int64_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::UTINYINT:
      TemplatedFullScan<uint8_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::USMALLINT:
      TemplatedFullScan<uint16_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::UINTEGER:
      TemplatedFullScan<uint32_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::UBIGINT:
      TemplatedFullScan<uint64_t>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::FLOAT:
      TemplatedFullScan<float>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::DOUBLE:
      TemplatedFullScan<double>(ptrs, col, count, col_offset);
      break;
    case PhysicalType::STRING: {
      auto out = FlatVector::GetData<string_t>(col);
      for (idx_t i = 0; i < count; i++) {
        auto row = ptrs[i];
        auto handle = Load<StringHandle>(row + col_offset);
        auto *bytes = reinterpret_cast<const char *>(row + handle.offset_);
        // Copy into col's own heap when the row bytes will not outlive this chunk (Fetch); otherwise
        // reference them in place (the scan keeps the page pinned across the pull).
        out[i] = copy_strings ? StringVector::AddString(col, bytes, handle.length_)
                              : string_t(bytes, handle.length_);
      }
      break;
    }
    default:
      throw NotImplementedException("RowOperations::FullScanColumn: unsupported physical type");
  }

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

}  // namespace bumblebee
