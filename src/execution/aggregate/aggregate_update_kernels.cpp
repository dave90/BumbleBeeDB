//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// aggregate_update_kernels.cpp
//
// Identification: src/execution/aggregate/aggregate_update_kernels.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/aggregate/aggregate_update_kernels.h"

#include "common/helper.h"
#include "type/value.h"

namespace bumblebee {

namespace {

/** The typed kernels are instantiated on two compile-time legs so the common case pays for nothing:
 * `HAS_SEL` reads the argument through a selection (a dictionary straight from Orrify — never
 * materialized) or directly (`data[i]`, flat); `HAS_NULLS` makes the all-valid instantiation a
 * branch-free loop. `addrs[i]` is each row's group row, so the state stores scatter by nature. */

/** @brief Bump every row's group count: COUNT(*), and every COUNT(x) case with nothing to skip. */
void UpdateCountStar(data_ptr_t *addrs, idx_t count, idx_t cnt_off) {
  for (idx_t i = 0; i < count; i++) {
    auto *addr = addrs[i];
    Store<int64_t>(Load<int64_t>(addr + cnt_off) + 1, addr + cnt_off);
  }
}

/** @brief COUNT(x) with NULLs present: bump only the rows whose argument is non-NULL. */
template <bool HAS_SEL>
void UpdateCountKernel(const SelectionVector *sel, const ValidityMask &validity, data_ptr_t *addrs, idx_t count,
                       idx_t cnt_off) {
  for (idx_t i = 0; i < count; i++) {
    const idx_t row = HAS_SEL ? sel->GetIndex(i) : i;
    if (validity.RowIsValid(row)) {
      auto *addr = addrs[i];
      Store<int64_t>(Load<int64_t>(addr + cnt_off) + 1, addr + cnt_off);
    }
  }
}

template <class T, bool HAS_NULLS, bool HAS_SEL>
void UpdateSumKernel(const T *data, const SelectionVector *sel, const ValidityMask &validity, data_ptr_t *addrs,
                     idx_t count, idx_t cnt_off, idx_t val_off) {
  for (idx_t i = 0; i < count; i++) {
    const idx_t row = HAS_SEL ? sel->GetIndex(i) : i;
    if constexpr (HAS_NULLS) {
      if (!validity.RowIsValid(row)) {
        continue;
      }
    }
    auto *addr = addrs[i];
    Store<int64_t>(Load<int64_t>(addr + cnt_off) + 1, addr + cnt_off);
    Store<double>(Load<double>(addr + val_off) + static_cast<double>(data[row]), addr + val_off);
  }
}

template <class T, bool MIN, bool HAS_NULLS, bool HAS_SEL>
void UpdateMinMaxKernel(const T *data, const SelectionVector *sel, const ValidityMask &validity, data_ptr_t *addrs,
                        idx_t count, idx_t cnt_off, idx_t val_off) {
  for (idx_t i = 0; i < count; i++) {
    const idx_t row = HAS_SEL ? sel->GetIndex(i) : i;
    if constexpr (HAS_NULLS) {
      if (!validity.RowIsValid(row)) {
        continue;
      }
    }
    auto *addr = addrs[i];
    const auto cnt = Load<int64_t>(addr + cnt_off);
    const auto x = static_cast<double>(data[row]);
    if (cnt == 0) {
      Store<double>(x, addr + val_off);
    } else {
      const auto cur = Load<double>(addr + val_off);
      Store<double>(MIN ? (x < cur ? x : cur) : (x > cur ? x : cur), addr + val_off);
    }
    Store<int64_t>(cnt + 1, addr + cnt_off);
  }
}

/** @brief The typed leg of one aggregate's update: pick the SUM / MIN / MAX kernel for element type `T`
 * and the right (all-valid vs masked) × (direct vs through-selection) instantiation. */
template <class T>
void UpdateNumericAggregate(AggregationType type, const_data_ptr_t raw, const SelectionVector *sel,
                            const ValidityMask &validity, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                            idx_t val_off) {
  const auto *data = reinterpret_cast<const T *>(raw);
  const bool all_valid = validity.AllValid();
  switch (type) {
    case AggregationType::AvgAggregate:  // AVG accumulates exactly like SUM (value + count); it only differs at finalize.
    case AggregationType::SumAggregate:
      if (sel != nullptr) {
        return all_valid ? UpdateSumKernel<T, false, true>(data, sel, validity, addrs, count, cnt_off, val_off)
                         : UpdateSumKernel<T, true, true>(data, sel, validity, addrs, count, cnt_off, val_off);
      }
      return all_valid ? UpdateSumKernel<T, false, false>(data, sel, validity, addrs, count, cnt_off, val_off)
                       : UpdateSumKernel<T, true, false>(data, sel, validity, addrs, count, cnt_off, val_off);
    case AggregationType::MinAggregate:
      if (sel != nullptr) {
        return all_valid
                   ? UpdateMinMaxKernel<T, true, false, true>(data, sel, validity, addrs, count, cnt_off, val_off)
                   : UpdateMinMaxKernel<T, true, true, true>(data, sel, validity, addrs, count, cnt_off, val_off);
      }
      return all_valid
                 ? UpdateMinMaxKernel<T, true, false, false>(data, sel, validity, addrs, count, cnt_off, val_off)
                 : UpdateMinMaxKernel<T, true, true, false>(data, sel, validity, addrs, count, cnt_off, val_off);
    default:
      if (sel != nullptr) {
        return all_valid
                   ? UpdateMinMaxKernel<T, false, false, true>(data, sel, validity, addrs, count, cnt_off, val_off)
                   : UpdateMinMaxKernel<T, false, true, true>(data, sel, validity, addrs, count, cnt_off, val_off);
      }
      return all_valid
                 ? UpdateMinMaxKernel<T, false, false, false>(data, sel, validity, addrs, count, cnt_off, val_off)
                 : UpdateMinMaxKernel<T, false, true, false>(data, sel, validity, addrs, count, cnt_off, val_off);
  }
}

/** @brief A CONSTANT non-NULL argument: the value (and its NULL check) is hoisted out of the loop —
 * every row folds the SAME x, only the group states differ. */
void UpdateConstant(AggregationType type, double x, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                    idx_t val_off) {
  switch (type) {
    case AggregationType::AvgAggregate:  // AVG accumulates like SUM.
    case AggregationType::SumAggregate:
      for (idx_t i = 0; i < count; i++) {
        auto *addr = addrs[i];
        Store<int64_t>(Load<int64_t>(addr + cnt_off) + 1, addr + cnt_off);
        Store<double>(Load<double>(addr + val_off) + x, addr + val_off);
      }
      return;
    default:
      const bool is_min = type == AggregationType::MinAggregate;
      for (idx_t i = 0; i < count; i++) {
        auto *addr = addrs[i];
        const auto cnt = Load<int64_t>(addr + cnt_off);
        if (cnt == 0) {
          Store<double>(x, addr + val_off);
        } else {
          const auto cur = Load<double>(addr + val_off);
          Store<double>(is_min ? (x < cur ? x : cur) : (x > cur ? x : cur), addr + val_off);
        }
        Store<int64_t>(cnt + 1, addr + cnt_off);
      }
  }
}

/** @brief Non-numeric argument: the boundary per-row path (matches the aggregates' numeric-only scope).
 * `GetValue` is encoding-aware, so this works for any vector type at logical row `i`. */
void UpdateFallbackAggregate(AggregationType type, Vector &arg, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                             idx_t val_off) {
  const bool is_min = type == AggregationType::MinAggregate;
  for (idx_t i = 0; i < count; i++) {
    const auto val = arg.GetValue(i);
    if (val.IsNull()) {
      continue;
    }
    const auto x = val.GetAs<double>();
    auto *addr = addrs[i];
    const auto cnt = Load<int64_t>(addr + cnt_off);
    if (type == AggregationType::SumAggregate || type == AggregationType::AvgAggregate) {
      Store<double>(Load<double>(addr + val_off) + x, addr + val_off);
    } else if (cnt == 0) {
      Store<double>(x, addr + val_off);
    } else {
      const auto cur = Load<double>(addr + val_off);
      Store<double>(is_min ? (x < cur ? x : cur) : (x > cur ? x : cur), addr + val_off);
    }
    Store<int64_t>(cnt + 1, addr + cnt_off);
  }
}

/** @brief Route SUM/MIN/MAX to the tight loop instantiated for the argument's physical type. */
void UpdateByPhysicalType(AggregationType type, Vector &arg, const_data_ptr_t data, const SelectionVector *sel,
                          const ValidityMask &validity, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                          idx_t val_off) {
  switch (arg.GetLogicalType().GetPhysicalType()) {
    case PhysicalType::TINYINT:
      return UpdateNumericAggregate<int8_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::SMALLINT:
      return UpdateNumericAggregate<int16_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::INTEGER:
      return UpdateNumericAggregate<int32_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::BIGINT:
      return UpdateNumericAggregate<int64_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::UTINYINT:
      return UpdateNumericAggregate<uint8_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::USMALLINT:
      return UpdateNumericAggregate<uint16_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::UINTEGER:
      return UpdateNumericAggregate<uint32_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::UBIGINT:
      return UpdateNumericAggregate<uint64_t>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::FLOAT:
      return UpdateNumericAggregate<float>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    case PhysicalType::DOUBLE:
      return UpdateNumericAggregate<double>(type, data, sel, validity, addrs, count, cnt_off, val_off);
    default:
      return UpdateFallbackAggregate(type, arg, addrs, count, cnt_off, val_off);
  }
}

}  // namespace

void UpdateOneAggregate(AggregationType type, Vector &arg, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                        idx_t val_off) {
  if (type == AggregationType::CountStarAggregate) {
    return UpdateCountStar(addrs, count, cnt_off);
  }
  if (arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
    const auto val = arg.GetValue(0);
    if (val.IsNull()) {
      return;  // a NULL constant contributes nothing (COUNT(*) was handled above)
    }
    if (type == AggregationType::CountAggregate) {
      return UpdateCountStar(addrs, count, cnt_off);  // every row is the same non-NULL value
    }
    return UpdateConstant(type, val.GetAs<double>(), addrs, count, cnt_off, val_off);
  }

  // A FLAT argument is read directly; anything else goes through Orrify — a dictionary hands over
  // its child data + selection with NO materialization, so the kernels do one pass reading
  // `data[sel[i]]` instead of a normalify-copy followed by a flat pass.
  const_data_ptr_t data = nullptr;
  const SelectionVector *sel = nullptr;
  const ValidityMask *validity = nullptr;
  VectorData vdata;
  if (arg.GetVectorType() == VectorType::FLAT_VECTOR) {
    data = FlatVector::GetData(arg);
    validity = &arg.Validity();
  } else {
    arg.Orrify(count, vdata);
    data = vdata.data_;
    sel = vdata.sel_;
    validity = vdata.validity_;
  }

  if (type == AggregationType::CountAggregate) {
    // COUNT never reads the data: all-valid counts every row no matter how they are selected.
    if (validity->AllValid()) {
      return UpdateCountStar(addrs, count, cnt_off);
    }
    return sel != nullptr ? UpdateCountKernel<true>(sel, *validity, addrs, count, cnt_off)
                          : UpdateCountKernel<false>(sel, *validity, addrs, count, cnt_off);
  }
  UpdateByPhysicalType(type, arg, data, sel, *validity, addrs, count, cnt_off, val_off);
}

}  // namespace bumblebee
