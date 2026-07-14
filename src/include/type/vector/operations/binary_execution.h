//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// binary_execution.h
//
// Identification: src/include/type/vector/operations/binary_execution.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"
#include "common/macros.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** @brief Call a functor's `Operation(left, right, idx, dataptr)`: an operator with state. */
struct GenericBinaryWrapper {
  template <class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right, idx_t idx, void *dataptr) -> RESULT_TYPE {
    return OP::Operation(left, right, idx, dataptr);
  }
};

/** @brief Call a functor's `Operation(left, right)`. The plain case. */
struct BinaryWrapper {
  template <class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    (void)dataptr;
    return OP::Operation(left, right);
  }
};

/**
 * The two-input kernel driver.
 *
 * Execute() produces a new value per row (`+`, `-`, `*`, ...); Select() filters rows
 * (`>`, `<`, `=`, ...) and returns the matching count.
 *
 * Both are specialized on the encodings of the two inputs: constant/constant collapses to
 * a single scalar evaluation, flat/flat is the tight loop, and anything else is orrified.
 */
struct BinaryExecution {
 protected:
  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static void ExecuteGenericLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                 RESULT_TYPE *__restrict result_data, const SelectionVector *__restrict lsel,
                                 const SelectionVector *__restrict rsel, idx_t count, void *dataptr) {
    for (idx_t i = 0; i < count; i++) {
      auto lentry = ldata[lsel->GetIndex(i)];
      auto rentry = rdata[rsel->GetIndex(i)];
      result_data[i] = OPWRAPPER::template Operation<OP, LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(lentry, rentry, i, dataptr);
    }
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static void ExecuteGeneric(Vector &left, Vector &right, Vector &result, idx_t count, void *dataptr) {
    VectorData ldata;
    VectorData rdata;

    left.Orrify(count, ldata);
    right.Orrify(count, rdata);

    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto *result_data = FlatVector::GetData<RESULT_TYPE>(result);
    ExecuteGenericLoop<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP>(
        reinterpret_cast<LEFT_TYPE *>(ldata.data_), reinterpret_cast<RIGHT_TYPE *>(rdata.data_), result_data,
        ldata.sel_, rdata.sel_, count, dataptr);
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP, bool LEFT_CONSTANT,
            bool RIGHT_CONSTANT>
  static void ExecuteFlatLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                              RESULT_TYPE *__restrict result_data, idx_t count, void *dataptr) {
    for (idx_t i = 0; i < count; i++) {
      auto lentry = ldata[LEFT_CONSTANT ? 0 : i];
      auto rentry = rdata[RIGHT_CONSTANT ? 0 : i];
      result_data[i] = OPWRAPPER::template Operation<OP, LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(lentry, rentry, i, dataptr);
    }
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP, bool LEFT_CONSTANT,
            bool RIGHT_CONSTANT>
  static void ExecuteFlat(Vector &left, Vector &right, Vector &result, idx_t count, void *dataptr) {
    BUMBLEBEE_ASSERT(!LEFT_CONSTANT || !RIGHT_CONSTANT, "both inputs constant: use ExecuteConstant");
    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto *result_data = FlatVector::GetData<RESULT_TYPE>(result);
    auto *ldata = FlatVector::GetData<LEFT_TYPE>(left);
    auto *rdata = FlatVector::GetData<RIGHT_TYPE>(right);
    ExecuteFlatLoop<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP, LEFT_CONSTANT, RIGHT_CONSTANT>(
        ldata, rdata, result_data, count, dataptr);
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static void ExecuteConstant(Vector &left, Vector &right, Vector &result, void *dataptr) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);

    auto *ldata = ConstantVector::GetData<LEFT_TYPE>(left);
    auto *rdata = ConstantVector::GetData<RIGHT_TYPE>(right);
    auto *result_data = ConstantVector::GetData<RESULT_TYPE>(result);

    *result_data = OPWRAPPER::template Operation<OP, LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(*ldata, *rdata, 0, dataptr);
  }

  /**
   * @brief Three-valued logic: a NULL on either input makes the result row NULL.
   *
   * Fast-path returns immediately when both inputs are all-valid, which is the common case.
   */
  static inline void PropagateBinaryValidity(Vector &left, Vector &right, Vector &result, idx_t count) {
    bool left_all_valid = left.Validity().AllValid();
    bool right_all_valid = right.Validity().AllValid();
    if (left_all_valid && right_all_valid) {
      return;
    }
    if (result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
      // Both operands are CONSTANT; if either is null, the single result value is null.
      ConstantVector::SetNull(result, true);
      return;
    }
    auto &rmask = FlatVector::Validity(result);
    VectorData lvd;
    VectorData rvd;
    left.Orrify(count, lvd);
    right.Orrify(count, rvd);
    const bool lav = lvd.validity_ == nullptr || lvd.validity_->AllValid();
    const bool rav = rvd.validity_ == nullptr || rvd.validity_->AllValid();
    bool allocated = false;  // allocate the result mask only on the first actual null
    for (idx_t i = 0; i < count; i++) {
      bool lnull = !lav && !lvd.validity_->RowIsValid(lvd.sel_->GetIndex(i));
      bool rnull = !rav && !rvd.validity_->RowIsValid(rvd.sel_->GetIndex(i));
      if (lnull || rnull) {
        if (!allocated) {
          rmask.EnsureWritable(count);
          allocated = true;
        }
        rmask.SetInvalidUnsafe(i);
      }
    }
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static inline void ExecuteStandard(Vector &left, Vector &right, Vector &result, idx_t count, void *dataptr) {
    auto left_vector_type = left.GetVectorType();
    auto right_vector_type = right.GetVectorType();
    if (left_vector_type == VectorType::CONSTANT_VECTOR && right_vector_type == VectorType::CONSTANT_VECTOR) {
      ExecuteConstant<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP>(left, right, result, dataptr);
    } else if (left_vector_type == VectorType::FLAT_VECTOR && right_vector_type == VectorType::CONSTANT_VECTOR) {
      ExecuteFlat<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP, false, true>(left, right, result, count, dataptr);
    } else if (left_vector_type == VectorType::CONSTANT_VECTOR && right_vector_type == VectorType::FLAT_VECTOR) {
      ExecuteFlat<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP, true, false>(left, right, result, count, dataptr);
    } else if (left_vector_type == VectorType::FLAT_VECTOR && right_vector_type == VectorType::FLAT_VECTOR) {
      ExecuteFlat<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP, false, false>(left, right, result, count, dataptr);
    } else {
      ExecuteGeneric<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OPWRAPPER, OP>(left, right, result, count, dataptr);
    }
    PropagateBinaryValidity(left, right, result, count);
  }

  // -- Select ---------------------------------------------------------------
  //
  // Three-valued logic for comparison: a NULL on either side makes the comparison UNKNOWN,
  // which is NOT TRUE. The row therefore lands in `false_sel`, so that the next branch of
  // an OR can still match it and the OR-eval invariant `count == true_count + false_count`
  // keeps holding.

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP>
  static auto SelectConstant(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                             SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
    auto *ldata = ConstantVector::GetData<LEFT_TYPE>(left);
    auto *rdata = ConstantVector::GetData<RIGHT_TYPE>(right);

    // Either constant NULL makes the comparison UNKNOWN for every row.
    bool either_null = ConstantVector::IsNull(left) || ConstantVector::IsNull(right);
    bool result = !either_null && OP::Operation(*ldata, *rdata);
    if (!result) {
      if (false_sel != nullptr) {
        for (idx_t i = 0; i < count; i++) {
          false_sel->SetIndex(i, sel->GetIndex(i));
        }
      }
      false_count = count;
      return 0;
    }
    if (true_sel != nullptr) {
      for (idx_t i = 0; i < count; i++) {
        true_sel->SetIndex(i, sel->GetIndex(i));
      }
    }
    false_count = 0;
    return count;
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool LEFT_CONSTANT, bool RIGHT_CONSTANT, bool HAS_TRUE_SEL,
            bool HAS_FALSE_SEL, bool HAS_NULLS>
  static inline auto SelectFlatLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                    const ValidityMask *lvalidity, const ValidityMask *rvalidity,
                                    const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                                    SelectionVector *false_sel, idx_t &false_count) -> idx_t {
    idx_t true_count = 0;
    false_count = 0;
    for (idx_t idx = 0; idx < count; idx++) {
      idx_t lidx = LEFT_CONSTANT ? 0 : sel->GetIndex(idx);
      idx_t ridx = RIGHT_CONSTANT ? 0 : sel->GetIndex(idx);
      bool comparison_result;
      if constexpr (HAS_NULLS) {
        bool both_valid = (LEFT_CONSTANT ? lvalidity->RowIsValid(0) : lvalidity->RowIsValid(lidx)) &&
                          (RIGHT_CONSTANT ? rvalidity->RowIsValid(0) : rvalidity->RowIsValid(ridx));
        comparison_result = both_valid && OP::Operation(ldata[lidx], rdata[ridx]);
      } else {
        comparison_result = OP::Operation(ldata[lidx], rdata[ridx]);
      }
      if (HAS_TRUE_SEL) {
        true_sel->SetIndex(true_count, sel->GetIndex(idx));
      }
      if (HAS_FALSE_SEL) {
        false_sel->SetIndex(false_count, sel->GetIndex(idx));
      }
      true_count += static_cast<idx_t>(comparison_result);
      false_count += static_cast<idx_t>(!comparison_result);
    }
    return true_count;
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool LEFT_CONSTANT, bool RIGHT_CONSTANT, bool HAS_NULLS>
  static inline auto SelectFlatSelSwitch(LEFT_TYPE *ldata, RIGHT_TYPE *rdata, const ValidityMask *lvalidity,
                                         const ValidityMask *rvalidity, const SelectionVector *sel, idx_t count,
                                         SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count)
      -> idx_t {
    if (true_sel != nullptr && false_sel != nullptr) {
      return SelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, LEFT_CONSTANT, RIGHT_CONSTANT, true, true, HAS_NULLS>(
          ldata, rdata, lvalidity, rvalidity, sel, count, true_sel, false_sel, false_count);
    }
    if (true_sel != nullptr) {
      return SelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, LEFT_CONSTANT, RIGHT_CONSTANT, true, false, HAS_NULLS>(
          ldata, rdata, lvalidity, rvalidity, sel, count, true_sel, false_sel, false_count);
    }
    if (false_sel != nullptr) {
      return SelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, LEFT_CONSTANT, RIGHT_CONSTANT, false, true, HAS_NULLS>(
          ldata, rdata, lvalidity, rvalidity, sel, count, true_sel, false_sel, false_count);
    }
    return SelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, LEFT_CONSTANT, RIGHT_CONSTANT, false, false, HAS_NULLS>(
        ldata, rdata, lvalidity, rvalidity, sel, count, true_sel, false_sel, false_count);
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
  static inline auto SelectFlat(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
    auto *ldata = FlatVector::GetData<LEFT_TYPE>(left);
    auto *rdata = FlatVector::GetData<RIGHT_TYPE>(right);
    // Validity lives on the Vector. For a CONSTANT vector that is a single-bit mask, read
    // at index 0 — which is what the LEFT_CONSTANT / RIGHT_CONSTANT branches above do.
    const auto &lvalidity = FlatVector::Validity(left);
    const auto &rvalidity = FlatVector::Validity(right);
    bool has_nulls = !lvalidity.AllValid() || !rvalidity.AllValid();
    if (has_nulls) {
      return SelectFlatSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, LEFT_CONSTANT, RIGHT_CONSTANT, true>(
          ldata, rdata, &lvalidity, &rvalidity, sel, count, true_sel, false_sel, false_count);
    }
    return SelectFlatSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, LEFT_CONSTANT, RIGHT_CONSTANT, false>(
        ldata, rdata, nullptr, nullptr, sel, count, true_sel, false_sel, false_count);
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool HAS_TRUE_SEL, bool HAS_FALSE_SEL, bool HAS_NULLS>
  static inline auto SelectGenericLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                       const SelectionVector *__restrict lsel, const SelectionVector *__restrict rsel,
                                       const ValidityMask *lvalidity, const ValidityMask *rvalidity,
                                       const SelectionVector *__restrict sel, idx_t count, SelectionVector *true_sel,
                                       SelectionVector *false_sel, idx_t &false_count) -> idx_t {
    idx_t true_count = 0;
    false_count = 0;
    for (idx_t i = 0; i < count; i++) {
      auto idx = sel->GetIndex(i);
      auto lindex = lsel->GetIndex(idx);
      auto rindex = rsel->GetIndex(idx);
      bool comparison_result;
      if constexpr (HAS_NULLS) {
        bool both_valid = lvalidity->RowIsValid(lindex) && rvalidity->RowIsValid(rindex);
        comparison_result = both_valid && OP::Operation(ldata[lindex], rdata[rindex]);
      } else {
        comparison_result = OP::Operation(ldata[lindex], rdata[rindex]);
      }
      if (HAS_TRUE_SEL) {
        true_sel->SetIndex(true_count, idx);
      }
      if (HAS_FALSE_SEL) {
        false_sel->SetIndex(false_count, idx);
      }
      true_count += static_cast<idx_t>(comparison_result);
      false_count += static_cast<idx_t>(!comparison_result);
    }
    return true_count;
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool HAS_NULLS>
  static inline auto SelectGenericLoopSelSwitch(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                                const SelectionVector *__restrict lsel,
                                                const SelectionVector *__restrict rsel, const ValidityMask *lvalidity,
                                                const ValidityMask *rvalidity,
                                                const SelectionVector *__restrict result_sel, idx_t count,
                                                SelectionVector *true_sel, SelectionVector *false_sel,
                                                idx_t &false_count) -> idx_t {
    if (true_sel != nullptr && false_sel != nullptr) {
      return SelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, true, true, HAS_NULLS>(
          ldata, rdata, lsel, rsel, lvalidity, rvalidity, result_sel, count, true_sel, false_sel, false_count);
    }
    if (true_sel != nullptr) {
      return SelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, true, false, HAS_NULLS>(
          ldata, rdata, lsel, rsel, lvalidity, rvalidity, result_sel, count, true_sel, false_sel, false_count);
    }
    if (false_sel != nullptr) {
      return SelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, false, true, HAS_NULLS>(
          ldata, rdata, lsel, rsel, lvalidity, rvalidity, result_sel, count, true_sel, false_sel, false_count);
    }
    return SelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, false, false, HAS_NULLS>(
        ldata, rdata, lsel, rsel, lvalidity, rvalidity, result_sel, count, true_sel, false_sel, false_count);
  }

  template <class LEFT_TYPE, class RIGHT_TYPE, class OP>
  static auto SelectGeneric(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                            SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
    VectorData ldata;
    VectorData rdata;

    left.Orrify(count, ldata);
    right.Orrify(count, rdata);

    // Orrify() populates validity_ as the source mask, to be read through sel_.
    bool has_nulls = (ldata.validity_ != nullptr && !ldata.validity_->AllValid()) ||
                     (rdata.validity_ != nullptr && !rdata.validity_->AllValid());
    if (has_nulls) {
      return SelectGenericLoopSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, true>(
          reinterpret_cast<LEFT_TYPE *>(ldata.data_), reinterpret_cast<RIGHT_TYPE *>(rdata.data_), ldata.sel_,
          rdata.sel_, ldata.validity_, rdata.validity_, sel, count, true_sel, false_sel, false_count);
    }
    return SelectGenericLoopSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, false>(
        reinterpret_cast<LEFT_TYPE *>(ldata.data_), reinterpret_cast<RIGHT_TYPE *>(rdata.data_), ldata.sel_, rdata.sel_,
        nullptr, nullptr, sel, count, true_sel, false_sel, false_count);
  }

 public:
  /**
   * @brief Apply a binary operation to `left` and `right`, writing `result`.
   *
   * Every combination of encodings (constant, flat, anything else) is handled. OP must
   * expose `static Operation(a, b)`.
   *
   * @param left The left input.
   * @param right The right input.
   * @param result The output. Its encoding is set by this call.
   * @param count The number of rows.
   */
  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
  static void Execute(Vector &left, Vector &right, Vector &result, idx_t count) {
    ExecuteStandard<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, BinaryWrapper, OP>(left, right, result, count, nullptr);
  }

  /** @brief Apply a stateful OP — one that also takes (idx, dataptr) — to every row. */
  template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
  static void GenericExecute(Vector &left, Vector &right, Vector &result, idx_t count, void *dataptr) {
    ExecuteStandard<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, GenericBinaryWrapper, OP>(left, right, result, count, dataptr);
  }

  /**
   * @brief Filter the rows where `left OP right` holds.
   *
   * A NULL on either side makes the row UNKNOWN, which is not TRUE: it is excluded from
   * `true_sel` and included in `false_sel`. OP must expose `static bool Operation(a, b)`.
   *
   * @param left The left input.
   * @param right The right input.
   * @param sel The rows to consider. Null means all of them.
   * @param count The number of rows.
   * @param true_sel Out: the matching rows. May be null.
   * @param false_sel Out: the non-matching rows. May be null.
   * @param false_count Out: the number of non-matching rows.
   * @return idx_t The number of matching rows.
   */
  template <class LEFT_TYPE, class RIGHT_TYPE, class OP>
  static auto Select(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                     SelectionVector *false_sel, idx_t &false_count) -> idx_t {
    if (sel == nullptr) {
      sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
    }
    if (left.GetVectorType() == VectorType::CONSTANT_VECTOR &&
        right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
      return SelectConstant<LEFT_TYPE, RIGHT_TYPE, OP>(left, right, sel, count, true_sel, false_sel, false_count);
    }
    if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::FLAT_VECTOR) {
      return SelectFlat<LEFT_TYPE, RIGHT_TYPE, OP, true, false>(left, right, sel, count, true_sel, false_sel,
                                                                false_count);
    }
    if (left.GetVectorType() == VectorType::FLAT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
      return SelectFlat<LEFT_TYPE, RIGHT_TYPE, OP, false, true>(left, right, sel, count, true_sel, false_sel,
                                                                false_count);
    }
    if (left.GetVectorType() == VectorType::FLAT_VECTOR && right.GetVectorType() == VectorType::FLAT_VECTOR) {
      return SelectFlat<LEFT_TYPE, RIGHT_TYPE, OP, false, false>(left, right, sel, count, true_sel, false_sel,
                                                                 false_count);
    }
    return SelectGeneric<LEFT_TYPE, RIGHT_TYPE, OP>(left, right, sel, count, true_sel, false_sel, false_count);
  }
};

}  // namespace bumblebee
