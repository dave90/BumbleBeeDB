//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// unary_execution.h
//
// Identification: src/include/type/vector/operations/unary_execution.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>

#include "common/config.h"
#include "type/null_value.h"
#include "type/vector/vector.h"

namespace bumblebee {

/** @brief Call a functor's `Operation(input)`. The plain case. */
struct UnaryOperatorWrapper {
  template <class OP, class INPUT_TYPE, class RESULT_TYPE>
  static inline auto Operation(INPUT_TYPE input, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    (void)dataptr;
    return OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input);
  }
};

/** @brief Call a lambda passed through `dataptr`. */
struct UnaryLambdaWrapper {
  template <class FUNC, class INPUT_TYPE, class RESULT_TYPE>
  static inline auto Operation(INPUT_TYPE input, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    auto *fun = reinterpret_cast<FUNC *>(dataptr);
    return (*fun)(input);
  }
};

/** @brief Call a functor's `Operation(input, dataptr)`: an operator with extra state. */
struct GenericUnaryWrapper {
  template <class OP, class INPUT_TYPE, class RESULT_TYPE>
  static inline auto Operation(INPUT_TYPE input, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    return OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, dataptr);
  }
};

/**
 * @brief Call a string operator, handing it the result Vector.
 *
 * A string result has to be written into the target's heap, so the operator needs the
 * Vector, not just the value. `dataptr` carries it.
 */
template <class OP>
struct UnaryStringOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    auto *vector = reinterpret_cast<Vector *>(dataptr);
    return OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, *vector);
  }
};

/**
 * The one-input kernel driver: Execute() computes a value per row, Select() filters rows.
 *
 * Every encoding of the input is handled: a CONSTANT input yields a CONSTANT result, a
 * FLAT input the tight loop, and anything else goes through Orrify().
 */
struct UnaryExecution {
 private:
  template <class INPUT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static inline void ExecuteLoop(INPUT_TYPE *__restrict ldata, RESULT_TYPE *__restrict result_data, idx_t count,
                                 const SelectionVector *__restrict sel_vector, void *dataptr) {
    for (idx_t i = 0; i < count; i++) {
      auto idx = sel_vector->GetIndex(i);
      result_data[i] = OPWRAPPER::template Operation<OP, INPUT_TYPE, RESULT_TYPE>(ldata[idx], i, dataptr);
    }
  }

  template <class INPUT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static inline void ExecuteFlat(INPUT_TYPE *__restrict ldata, RESULT_TYPE *__restrict result_data, idx_t count,
                                 void *dataptr) {
    for (idx_t i = 0; i < count; i++) {
      result_data[i] = OPWRAPPER::template Operation<OP, INPUT_TYPE, RESULT_TYPE>(ldata[i], i, dataptr);
    }
  }

  /**
   * @brief Three-valued logic: a NULL input row makes the result row NULL.
   *
   * Skipped entirely in the all-valid case, which is most callers.
   */
  static inline void PropagateUnaryValidity(Vector &input, Vector &result, idx_t count) {
    if (input.Validity().AllValid()) {
      return;
    }
    if (result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
      ConstantVector::SetNull(result, true);
      return;
    }
    VectorData vd;
    input.Orrify(count, vd);
    auto &rmask = FlatVector::Validity(result);
    bool allocated = false;  // allocate the result mask only on the first actual null
    for (idx_t i = 0; i < count; i++) {
      if (!vd.validity_->RowIsValid(vd.sel_->GetIndex(i))) {
        if (!allocated) {
          rmask.EnsureWritable(count);
          allocated = true;
        }
        rmask.SetInvalidUnsafe(i);
      }
    }
  }

  template <class INPUT_TYPE, class RESULT_TYPE, class OPWRAPPER, class OP>
  static inline void ExecuteStandard(Vector &input, Vector &result, idx_t count, void *dataptr) {
    // A NULL row never reaches OP: the result is NULL whatever the input bytes are, and
    // skipping it avoids spurious work — a string->int cast of a NULL cell must not report
    // a parse error. The defensive NullValue fill keeps the skipped slot from holding
    // garbage; PropagateUnaryValidity below marks the row NULL. The all-valid common case
    // keeps the branch-free fast path.
    bool has_nulls = !input.Validity().AllValid();
    switch (input.GetVectorType()) {
      case VectorType::CONSTANT_VECTOR: {
        result.SetVectorType(VectorType::CONSTANT_VECTOR);
        auto *result_data = ConstantVector::GetData<RESULT_TYPE>(result);
        auto *ldata = ConstantVector::GetData<INPUT_TYPE>(input);

        if (has_nulls && !input.RowIsValid(0)) {
          *result_data = NullValue<RESULT_TYPE>();
        } else {
          *result_data = OPWRAPPER::template Operation<OP, INPUT_TYPE, RESULT_TYPE>(*ldata, 0, dataptr);
        }
        break;
      }
      case VectorType::FLAT_VECTOR: {
        result.SetVectorType(VectorType::FLAT_VECTOR);
        auto *result_data = FlatVector::GetData<RESULT_TYPE>(result);
        auto *ldata = FlatVector::GetData<INPUT_TYPE>(input);

        if (has_nulls) {
          const ValidityMask &imask = FlatVector::Validity(input);
          for (idx_t i = 0; i < count; i++) {
            if (imask.RowIsValid(i)) {
              result_data[i] = OPWRAPPER::template Operation<OP, INPUT_TYPE, RESULT_TYPE>(ldata[i], i, dataptr);
            } else {
              result_data[i] = NullValue<RESULT_TYPE>();
            }
          }
        } else {
          ExecuteFlat<INPUT_TYPE, RESULT_TYPE, OPWRAPPER, OP>(ldata, result_data, count, dataptr);
        }
        break;
      }
      default: {
        VectorData vdata;
        input.Orrify(count, vdata);

        result.SetVectorType(VectorType::FLAT_VECTOR);
        auto *result_data = FlatVector::GetData<RESULT_TYPE>(result);
        auto *ldata = reinterpret_cast<INPUT_TYPE *>(vdata.data_);

        if (has_nulls) {
          const ValidityMask *imask = vdata.validity_;
          for (idx_t i = 0; i < count; i++) {
            idx_t sidx = vdata.sel_->GetIndex(i);
            if (imask->RowIsValid(sidx)) {
              result_data[i] = OPWRAPPER::template Operation<OP, INPUT_TYPE, RESULT_TYPE>(ldata[sidx], i, dataptr);
            } else {
              result_data[i] = NullValue<RESULT_TYPE>();
            }
          }
        } else {
          ExecuteLoop<INPUT_TYPE, RESULT_TYPE, OPWRAPPER, OP>(ldata, result_data, count, vdata.sel_, dataptr);
        }
        break;
      }
    }
    PropagateUnaryValidity(input, result, count);
  }

  template <class INPUT_TYPE, class OPWRAPPER, class OP>
  static auto SelectConstant(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                             SelectionVector *false_sel, idx_t &false_count, void *dataptr) -> idx_t {
    auto *idata = ConstantVector::GetData<INPUT_TYPE>(input);

    if (!OPWRAPPER::template Operation<OP, INPUT_TYPE, bool>(*idata, 0, dataptr)) {
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

  template <class INPUT_TYPE, class OPWRAPPER, class OP, bool HAS_TRUE_SEL, bool HAS_FALSE_SEL>
  static auto SelectFlatLoop(INPUT_TYPE *__restrict idata, const SelectionVector *sel, idx_t count,
                             SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count, void *dataptr)
      -> idx_t {
    idx_t true_count = 0;
    false_count = 0;
    for (idx_t idx = 0; idx < count; idx++) {
      auto comparison_result =
          OPWRAPPER::template Operation<OP, INPUT_TYPE, bool>(idata[sel->GetIndex(idx)], 0, dataptr);
      // Write the index unconditionally and only bump the counter on a match: branchless.
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

  template <class INPUT_TYPE, class OPWRAPPER, class OP>
  static auto SelectFlat(INPUT_TYPE *__restrict idata, const SelectionVector *sel, idx_t count,
                         SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count, void *dataptr)
      -> idx_t {
    if (true_sel != nullptr && false_sel != nullptr) {
      return SelectFlatLoop<INPUT_TYPE, OPWRAPPER, OP, true, true>(idata, sel, count, true_sel, false_sel, false_count,
                                                                   dataptr);
    }
    if (true_sel != nullptr) {
      return SelectFlatLoop<INPUT_TYPE, OPWRAPPER, OP, true, false>(idata, sel, count, true_sel, false_sel, false_count,
                                                                    dataptr);
    }
    if (false_sel != nullptr) {
      return SelectFlatLoop<INPUT_TYPE, OPWRAPPER, OP, false, true>(idata, sel, count, true_sel, false_sel, false_count,
                                                                    dataptr);
    }
    return SelectFlatLoop<INPUT_TYPE, OPWRAPPER, OP, false, false>(idata, sel, count, true_sel, false_sel, false_count,
                                                                   dataptr);
  }

  template <class INPUT_TYPE, class OPWRAPPER, class OP, bool HAS_TRUE_SEL, bool HAS_FALSE_SEL>
  static auto SelectGenericLoop(INPUT_TYPE *__restrict idata, const SelectionVector *sel, const SelectionVector *isel,
                                idx_t count, SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count,
                                void *dataptr) -> idx_t {
    idx_t true_count = 0;
    false_count = 0;
    for (idx_t i = 0; i < count; i++) {
      auto idx = sel->GetIndex(i);
      auto iindex = isel->GetIndex(idx);
      auto comparison_result = OPWRAPPER::template Operation<OP, INPUT_TYPE, bool>(idata[iindex], 0, dataptr);
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

  template <class INPUT_TYPE, class OPWRAPPER, class OP>
  static auto SelectGeneric(INPUT_TYPE *__restrict idata, const SelectionVector *sel, const SelectionVector *isel,
                            idx_t count, SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count,
                            void *dataptr) -> idx_t {
    if (true_sel != nullptr && false_sel != nullptr) {
      return SelectGenericLoop<INPUT_TYPE, OPWRAPPER, OP, true, true>(idata, sel, isel, count, true_sel, false_sel,
                                                                      false_count, dataptr);
    }
    if (true_sel != nullptr) {
      return SelectGenericLoop<INPUT_TYPE, OPWRAPPER, OP, true, false>(idata, sel, isel, count, true_sel, false_sel,
                                                                       false_count, dataptr);
    }
    if (false_sel != nullptr) {
      return SelectGenericLoop<INPUT_TYPE, OPWRAPPER, OP, false, true>(idata, sel, isel, count, true_sel, false_sel,
                                                                       false_count, dataptr);
    }
    return SelectGenericLoop<INPUT_TYPE, OPWRAPPER, OP, false, false>(idata, sel, isel, count, true_sel, false_sel,
                                                                      false_count, dataptr);
  }

  template <class INPUT_TYPE, class OPWRAPPER, class OP>
  static inline auto ExecuteSelect(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                                   SelectionVector *false_sel, idx_t &false_count, void *dataptr) -> idx_t {
    if (sel == nullptr) {
      sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
    }
    switch (input.GetVectorType()) {
      case VectorType::CONSTANT_VECTOR:
        return SelectConstant<INPUT_TYPE, OPWRAPPER, OP>(input, sel, count, true_sel, false_sel, false_count, dataptr);
      case VectorType::FLAT_VECTOR: {
        auto *idata = FlatVector::GetData<INPUT_TYPE>(input);
        return SelectFlat<INPUT_TYPE, OPWRAPPER, OP>(idata, sel, count, true_sel, false_sel, false_count, dataptr);
      }
      default: {
        VectorData vdata;
        input.Orrify(count, vdata);
        auto *idata = reinterpret_cast<INPUT_TYPE *>(vdata.data_);
        return SelectGeneric<INPUT_TYPE, OPWRAPPER, OP>(idata, sel, vdata.sel_, count, true_sel, false_sel, false_count,
                                                        dataptr);
      }
    }
  }

 public:
  /**
   * @brief Filter the rows of `input` by a predicate lambda.
   *
   * @param input The vector to filter.
   * @param sel The rows to consider. Null means all of them.
   * @param count The number of rows.
   * @param true_sel Out: the matching rows. May be null.
   * @param false_sel Out: the non-matching rows. May be null.
   * @param false_count Out: the number of non-matching rows.
   * @param fun The predicate.
   * @return idx_t The number of matching rows.
   */
  template <class INPUT_TYPE, class FUNC = std::function<bool(INPUT_TYPE)>>
  static auto Select(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                     SelectionVector *false_sel, idx_t &false_count, FUNC fun) -> idx_t {
    return ExecuteSelect<INPUT_TYPE, UnaryLambdaWrapper, FUNC>(input, sel, count, true_sel, false_sel, false_count,
                                                               reinterpret_cast<void *>(&fun));
  }

  /** @brief Apply OP to every row of `input`, writing `result`. */
  template <class INPUT_TYPE, class RESULT_TYPE, class OP>
  static void Execute(Vector &input, Vector &result, idx_t count) {
    ExecuteStandard<INPUT_TYPE, RESULT_TYPE, UnaryOperatorWrapper, OP>(input, result, count, nullptr);
  }

  /** @brief Apply a lambda to every row of `input`, writing `result`. */
  template <class INPUT_TYPE, class RESULT_TYPE, class FUNC = std::function<RESULT_TYPE(INPUT_TYPE)>>
  static void Execute(Vector &input, Vector &result, idx_t count, FUNC fun) {
    ExecuteStandard<INPUT_TYPE, RESULT_TYPE, UnaryLambdaWrapper, FUNC>(input, result, count,
                                                                       reinterpret_cast<void *>(&fun));
  }

  /** @brief Apply a stateful OP — one that also takes `dataptr` — to every row. */
  template <class INPUT_TYPE, class RESULT_TYPE, class OP>
  static void GenericExecute(Vector &input, Vector &result, idx_t count, void *dataptr) {
    ExecuteStandard<INPUT_TYPE, RESULT_TYPE, GenericUnaryWrapper, OP>(input, result, count, dataptr);
  }

  /** @brief Apply a string-producing OP, handing it `result` so it can use its heap. */
  template <class INPUT_TYPE, class RESULT_TYPE, class OP>
  static void ExecuteString(Vector &input, Vector &result, idx_t count) {
    GenericExecute<string_t, string_t, UnaryStringOperator<OP>>(input, result, count,
                                                                reinterpret_cast<void *>(&result));
  }
};

}  // namespace bumblebee
