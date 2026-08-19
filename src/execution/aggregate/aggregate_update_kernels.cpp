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

#include "common/exception.h"
#include "common/helper.h"
#include "execution/aggregate/aggregate_semantics.h"
#include "type/physical_type_dispatch.h"
#include "type/value.h"

namespace bumblebee {

/** @brief Bump a group row's count slot by `n`. */
static inline void BumpCount(data_ptr_t addr, idx_t cnt_off, int64_t n = 1) {
  Store<int64_t>(Load<int64_t>(addr + cnt_off) + n, addr + cnt_off);
}

/**
 * @brief Lift "are there NULLs" and "is the argument read through a selection" to template
 *        arguments, then invoke `fn.operator()<HAS_NULLS, HAS_SEL>()`.
 *
 * The kernels below want both as compile-time constants — that is what makes the common all-valid,
 * flat case a branch-free loop. Doing it by hand costs a four-way ternary per aggregate; this keeps
 * the four instantiations while naming the choice once. The lambda inlines away entirely.
 */
template <class FN>
static inline void DispatchNullsAndSelection(bool has_nulls, bool has_sel, FN &&fn) {
  if (has_sel) {
    return has_nulls ? fn.template operator()<true, true>() : fn.template operator()<false, true>();
  }
  return has_nulls ? fn.template operator()<true, false>() : fn.template operator()<false, false>();
}

/** The typed kernels are instantiated on two compile-time legs so the common case pays for nothing:
 * `HAS_SEL` reads the argument through a selection (a dictionary straight from Orrify — never
 * materialized) or directly (`data[i]`, flat); `HAS_NULLS` makes the all-valid instantiation a
 * branch-free loop. `addrs[i]` is each row's group row, so the state stores scatter by nature. */

/** @brief Bump every row's group count: COUNT(*), and every COUNT(x) case with nothing to skip. */
static void UpdateCountStar(data_ptr_t *addrs, idx_t count, idx_t cnt_off) {
  for (idx_t i = 0; i < count; i++) {
    BumpCount(addrs[i], cnt_off);
  }
}

/** @brief COUNT(x) with NULLs present: bump only the rows whose argument is non-NULL. */
template <bool HAS_SEL>
static void UpdateCountKernel(const SelectionVector *sel, const ValidityMask &validity, data_ptr_t *addrs, idx_t count,
                              idx_t cnt_off) {
  for (idx_t i = 0; i < count; i++) {
    const idx_t row = HAS_SEL ? sel->GetIndex(i) : i;
    if (validity.RowIsValid(row)) {
      BumpCount(addrs[i], cnt_off);
    }
  }
}

template <class T, bool HAS_NULLS, bool HAS_SEL>
static void UpdateSumKernel(const T *data, const SelectionVector *sel, const ValidityMask &validity, data_ptr_t *addrs,
                            idx_t count, idx_t cnt_off, idx_t val_off) {
  for (idx_t i = 0; i < count; i++) {
    const idx_t row = HAS_SEL ? sel->GetIndex(i) : i;
    if constexpr (HAS_NULLS) {
      if (!validity.RowIsValid(row)) {
        continue;
      }
    }
    auto *addr = addrs[i];
    BumpCount(addr, cnt_off);
    Store<double>(Load<double>(addr + val_off) + static_cast<double>(data[row]), addr + val_off);
  }
}

template <class T, bool MIN, bool HAS_NULLS, bool HAS_SEL>
static void UpdateMinMaxKernel(const T *data, const SelectionVector *sel, const ValidityMask &validity,
                               data_ptr_t *addrs, idx_t count, idx_t cnt_off, idx_t val_off) {
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
    // cnt == 0 means "no extreme yet", so the first value seeds the state instead of folding.
    Store<double>(cnt == 0 ? x : FoldExtreme<MIN>(Load<double>(addr + val_off), x), addr + val_off);
    Store<int64_t>(cnt + 1, addr + cnt_off);
  }
}

/** @brief The typed leg of one aggregate's update: pick the SUM / MIN / MAX kernel for element type `T`
 * and the right (all-valid vs masked) × (direct vs through-selection) instantiation. */
template <class T>
static void UpdateNumericAggregate(AggregationType type, const_data_ptr_t raw, const SelectionVector *sel,
                                   const ValidityMask &validity, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                                   idx_t val_off) {
  const auto *data = reinterpret_cast<const T *>(raw);
  const bool has_nulls = !validity.AllValid();
  const bool has_sel = sel != nullptr;

  // COUNT(*) and COUNT(x) are answered before we get here, so only the four value aggregates remain.
  if (IsSumLike(type)) {
    return DispatchNullsAndSelection(has_nulls, has_sel, [&]<bool HAS_NULLS, bool HAS_SEL>() {
      UpdateSumKernel<T, HAS_NULLS, HAS_SEL>(data, sel, validity, addrs, count, cnt_off, val_off);
    });
  }
  if (type == AggregationType::MinAggregate) {
    return DispatchNullsAndSelection(has_nulls, has_sel, [&]<bool HAS_NULLS, bool HAS_SEL>() {
      UpdateMinMaxKernel<T, true, HAS_NULLS, HAS_SEL>(data, sel, validity, addrs, count, cnt_off, val_off);
    });
  }
  if (type == AggregationType::MaxAggregate) {
    return DispatchNullsAndSelection(has_nulls, has_sel, [&]<bool HAS_NULLS, bool HAS_SEL>() {
      UpdateMinMaxKernel<T, false, HAS_NULLS, HAS_SEL>(data, sel, validity, addrs, count, cnt_off, val_off);
    });
  }
  throw NotImplementedException(fmt::format("grouped aggregation: unsupported aggregate {}", type));
}

/** @brief A CONSTANT non-NULL argument: the value (and its NULL check) is hoisted out of the loop —
 * every row folds the SAME x, only the group states differ. */
static void UpdateConstant(AggregationType type, double x, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                           idx_t val_off) {
  if (IsSumLike(type)) {
    for (idx_t i = 0; i < count; i++) {
      auto *addr = addrs[i];
      BumpCount(addr, cnt_off);
      Store<double>(Load<double>(addr + val_off) + x, addr + val_off);
    }
    return;
  }
  for (idx_t i = 0; i < count; i++) {
    auto *addr = addrs[i];
    const auto cnt = Load<int64_t>(addr + cnt_off);
    Store<double>(cnt == 0 ? x : FoldExtreme(type, Load<double>(addr + val_off), x), addr + val_off);
    Store<int64_t>(cnt + 1, addr + cnt_off);
  }
}

/** @brief Non-numeric argument: the boundary per-row path (matches the aggregates' numeric-only scope).
 * `GetValue` is encoding-aware, so this works for any vector type at logical row `i`. */
static void UpdateFallbackAggregate(AggregationType type, Vector &arg, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                                    idx_t val_off) {
  const bool sum_like = IsSumLike(type);  // hoisted: it cannot change across the rows
  for (idx_t i = 0; i < count; i++) {
    const auto val = arg.GetValue(i);
    if (val.IsNull()) {
      continue;
    }
    const auto x = val.GetAs<double>();
    auto *addr = addrs[i];
    const auto cnt = Load<int64_t>(addr + cnt_off);
    const auto cur = Load<double>(addr + val_off);
    Store<double>(sum_like ? cur + x : (cnt == 0 ? x : FoldExtreme(type, cur, x)), addr + val_off);
    Store<int64_t>(cnt + 1, addr + cnt_off);
  }
}

/** @brief Route SUM/MIN/MAX to the tight loop instantiated for the argument's physical type. */
static void UpdateByPhysicalType(AggregationType type, Vector &arg, const_data_ptr_t data, const SelectionVector *sel,
                                 const ValidityMask &validity, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                                 idx_t val_off) {
  const auto ptype = arg.GetLogicalType().GetPhysicalType();
  DispatchNumericPhysicalType(
      ptype, [&]<class T>() { UpdateNumericAggregate<T>(type, data, sel, validity, addrs, count, cnt_off, val_off); },
      [&]() { UpdateFallbackAggregate(type, arg, addrs, count, cnt_off, val_off); });
}

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
