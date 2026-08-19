//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_comparison.cpp
//
// Identification: src/type/vector/operations/vector_comparison.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <string>
#include <type_traits>

#include "common/exception.h"
#include "common/helper.h"
#include "common/macros.h"
#include "type/date.h"
#include "type/physical_type_dispatch.h"
#include "type/vector/operations/binary_execution.h"
#include "type/vector/operator/comparison_operators.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/** @brief The fast path: both sides share a physical type, so no promotion is needed. */
template <class OP>
static auto TemplatedSelectOperationSwitchEqualType(Vector &left, Vector &right, const SelectionVector *sel,
                                                    idx_t count, SelectionVector *true_sel, SelectionVector *false_sel,
                                                    idx_t &false_count) -> idx_t {
  BUMBLEBEE_ASSERT(left.GetType() == right.GetType(), "the two sides must share a physical type");
  return DispatchNumericAndStringPhysicalType(
      left.GetType(),
      [&]<class T>() {
        return BinaryExecution::Select<T, T, OP>(left, right, sel, count, true_sel, false_sel, false_count);
      },
      [&]() -> idx_t {
        throw NotImplementedException(
            fmt::format("comparison: unsupported type {}", LogicalType::NameOf(left.GetType())));
      });
}

// The two comparison policy functors below stay in an anonymous namespace: they are template
// arguments to kernels instantiated over the full type cross-product, and a type has no `static`
// spelling for internal linkage ([basic.link]). Promoting them to `bumblebee` flips every
// instantiation naming them from internal to weak external linkage, which costs this file
// 7.4 MB -> 11.4 MB of object code and 710 -> 12,854 exported symbols. See the longer note in
// vector_arith.cpp, which has the same property.
namespace {

/** @brief Promote both sides to COMMON_TYPE, then compare. */
template <class LEFT_TYPE, class RIGHT_TYPE, class COMMON_TYPE, class OP>
struct ComparisonCommonCast {
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right) -> bool {
    return OP::Operation(static_cast<COMMON_TYPE>(left), static_cast<COMMON_TYPE>(right));
  }
};

/** @brief Parse the string side as a DATE, then compare. INVERSE flips the operand order. */
template <class OP, bool INVERSE>
struct StringDateCast {
  static inline auto Operation(const string_t &left, date_t right) -> bool {
    date_t date_left;
    idx_t pos;
    if (!Date::TryConvertDate(left.CStr(), left.Length(), pos, date_left, true)) {
      throw Exception(ExceptionType::CONVERSION, fmt::format("error parsing string to date: {}", left.GetString()));
    }
    if (INVERSE) {
      return OP::Operation(right, date_left);
    }
    return OP::Operation(date_left, right);
  }
};

}  // namespace

/**
 * @brief Compare when at least one side is a DECIMAL, by bringing both onto one scale.
 *
 * A DECIMAL is a scaled integer, so its raw bytes are not comparable against anything —
 * not against a plain number, and NOT against another DECIMAL of a different scale, where
 * 1.00 (scale 2, raw 100) and 1.0 (scale 1, raw 10) would compare unequal. Both sides are
 * cast to the common DECIMAL type first, and only then compared.
 */
template <class OP>
static auto TemplatedSelectOperationDecimal(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                            SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count)
    -> idx_t {
  Vector l_sel_vector(left);
  Vector r_sel_vector(right);

  // The inner comparison runs on the SLICED vectors, so it is called with a null selection
  // and therefore writes SLICED indices (0..count-1) into true_sel / false_sel, not the
  // original chunk indices from `sel`. Save the original indices first so we can map back.
  //
  // `sel` may alias the false_sel buffer (an OR-eval loop feeds one iteration's false_sel
  // in as the next iteration's sel), so the inner call can overwrite it — saved_buf must be
  // an independent copy.
  const bool need_remap = sel != nullptr && sel->GetData() != nullptr;
  sel_t saved_buf[STANDARD_VECTOR_SIZE];
  if (need_remap) {
    BUMBLEBEE_ASSERT(count <= STANDARD_VECTOR_SIZE, "decimal comparison: count exceeds a vector");
    for (idx_t i = 0; i < count; i++) {
      saved_buf[i] = static_cast<sel_t>(sel->GetIndex(i));
    }
  }

  if (sel != nullptr) {
    l_sel_vector.Slice(*sel, count);
    r_sel_vector.Slice(*sel, count);
  }

  // The type both sides are brought to. For DECIMAL vs DECIMAL that is the wider scale; for
  // DECIMAL vs anything else it is the DECIMAL itself.
  const bool left_is_decimal = l_sel_vector.GetLogicalTypeId() == LogicalTypeId::DECIMAL;
  const bool right_is_decimal = r_sel_vector.GetLogicalTypeId() == LogicalTypeId::DECIMAL;
  BUMBLEBEE_ASSERT(left_is_decimal || right_is_decimal, "one side must be a DECIMAL");
  LogicalType target = left_is_decimal && right_is_decimal
                           ? LogicalType::CommonType(l_sel_vector.GetLogicalType(), r_sel_vector.GetLogicalType())
                           : (left_is_decimal ? l_sel_vector.GetLogicalType() : r_sel_vector.GetLogicalType());

  // TryCast carries the source validity 1:1 (UnaryExecution::PropagateUnaryValidity), so
  // the cast vectors end up with the right mask without any fix-up here.
  const idx_t capacity = MaxValue<idx_t>(count, 1);
  Vector l_cast(target, capacity);
  Vector r_cast(target, capacity);
  Vector *lhs = &l_sel_vector;
  Vector *rhs = &r_sel_vector;
  std::string error_msg;
  if (l_sel_vector.GetLogicalType() != target) {
    if (!VectorOperations::TryCast(l_sel_vector, l_cast, count, &error_msg)) {
      throw Exception(ExceptionType::CONVERSION, "comparison: cannot cast the operand to DECIMAL: " + error_msg);
    }
    lhs = &l_cast;
  }
  if (r_sel_vector.GetLogicalType() != target) {
    if (!VectorOperations::TryCast(r_sel_vector, r_cast, count, &error_msg)) {
      throw Exception(ExceptionType::CONVERSION, "comparison: cannot cast the operand to DECIMAL: " + error_msg);
    }
    rhs = &r_cast;
  }
  idx_t true_count =
      TemplatedSelectOperationSwitchEqualType<OP>(*lhs, *rhs, nullptr, count, true_sel, false_sel, false_count);

  // Map the sliced indices (0..count-1) back to the original chunk indices.
  if (need_remap) {
    if (true_sel != nullptr) {
      for (idx_t i = 0; i < true_count; i++) {
        true_sel->SetIndex(i, saved_buf[true_sel->GetIndex(i)]);
      }
    }
    if (false_sel != nullptr) {
      for (idx_t i = 0; i < false_count; i++) {
        false_sel->SetIndex(i, saved_buf[false_sel->GetIndex(i)]);
      }
    }
  }
  return true_count;
}

/**
 * @brief Pick the type both sides are promoted to, and compare there.
 *
 * The integers all collapse to int64; the floating-point types to double. Comparing the
 * two operands in their own types would be wrong: `(int32_t)-1 > (uint32_t)0` is TRUE
 * under C's usual arithmetic conversions, and FALSE in SQL.
 */
template <class LEFT_TYPE, class RIGHT_TYPE, class OP>
static auto TemplatedSelectOperationSwitchCommon(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                                 SelectionVector *true_sel, SelectionVector *false_sel,
                                                 idx_t &false_count) -> idx_t {
  auto common_type = LogicalType::CommonType(left.GetType(), right.GetType());

  switch (common_type.GetPhysicalType()) {
    case PhysicalType::TINYINT:
    case PhysicalType::SMALLINT:
    case PhysicalType::INTEGER:
    case PhysicalType::UTINYINT:
    case PhysicalType::USMALLINT:
    case PhysicalType::UINTEGER:
    case PhysicalType::UBIGINT:
    case PhysicalType::BIGINT:
      return BinaryExecution::Select<LEFT_TYPE, RIGHT_TYPE, ComparisonCommonCast<LEFT_TYPE, RIGHT_TYPE, int64_t, OP>>(
          left, right, sel, count, true_sel, false_sel, false_count);
    case PhysicalType::FLOAT:
    case PhysicalType::DOUBLE:
      return BinaryExecution::Select<LEFT_TYPE, RIGHT_TYPE, ComparisonCommonCast<LEFT_TYPE, RIGHT_TYPE, double, OP>>(
          left, right, sel, count, true_sel, false_sel, false_count);
    default:
      throw NotImplementedException(
          fmt::format("comparison: unsupported common type {}", LogicalType::NameOf(common_type.GetPhysicalType())));
  }
}

template <class LEFT_TYPE, class OP>
static auto TemplatedSelectOperationSwitchRight(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                                SelectionVector *true_sel, SelectionVector *false_sel,
                                                idx_t &false_count) -> idx_t {
  switch (right.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, int8_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                         false_count);
    case LogicalTypeId::SMALLINT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, int16_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                          false_count);
    case LogicalTypeId::INTEGER:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, int32_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                          false_count);
    case LogicalTypeId::BIGINT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, int64_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                          false_count);
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, uint8_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                          false_count);
    case LogicalTypeId::USMALLINT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, uint16_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                           false_count);
    case LogicalTypeId::UINTEGER:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, uint32_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                           false_count);
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, uint64_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                                           false_count);
    case LogicalTypeId::FLOAT:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, float, OP>(left, right, sel, count, true_sel, false_sel,
                                                                        false_count);
    case LogicalTypeId::DOUBLE:
      return TemplatedSelectOperationSwitchCommon<LEFT_TYPE, double, OP>(left, right, sel, count, true_sel, false_sel,
                                                                         false_count);
    case LogicalTypeId::DECIMAL:
      return TemplatedSelectOperationDecimal<OP>(left, right, sel, count, true_sel, false_sel, false_count);
    default:
      throw NotImplementedException(
          fmt::format("comparison: unsupported right type {}", LogicalType::NameOf(right.GetLogicalTypeId())));
  }
}

/** @brief The slow path: the two sides have different types, so both must be dispatched. */
template <class OP>
static auto TemplatedSelectOperationSwitchLeft(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                               SelectionVector *true_sel, SelectionVector *false_sel,
                                               idx_t &false_count) -> idx_t {
  switch (left.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      return TemplatedSelectOperationSwitchRight<int8_t, OP>(left, right, sel, count, true_sel, false_sel, false_count);
    case LogicalTypeId::SMALLINT:
      return TemplatedSelectOperationSwitchRight<int16_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                              false_count);
    case LogicalTypeId::INTEGER:
      return TemplatedSelectOperationSwitchRight<int32_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                              false_count);
    case LogicalTypeId::BIGINT:
      return TemplatedSelectOperationSwitchRight<int64_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                              false_count);
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
      return TemplatedSelectOperationSwitchRight<uint8_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                              false_count);
    case LogicalTypeId::USMALLINT:
      return TemplatedSelectOperationSwitchRight<uint16_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                               false_count);
    case LogicalTypeId::UINTEGER:
      return TemplatedSelectOperationSwitchRight<uint32_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                               false_count);
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      return TemplatedSelectOperationSwitchRight<uint64_t, OP>(left, right, sel, count, true_sel, false_sel,
                                                               false_count);
    case LogicalTypeId::FLOAT:
      return TemplatedSelectOperationSwitchRight<float, OP>(left, right, sel, count, true_sel, false_sel, false_count);
    case LogicalTypeId::DOUBLE:
      return TemplatedSelectOperationSwitchRight<double, OP>(left, right, sel, count, true_sel, false_sel, false_count);
    case LogicalTypeId::STRING: {
      if (right.GetLogicalTypeId() == LogicalTypeId::DATE) {
        return BinaryExecution::Select<string_t, date_t, StringDateCast<OP, false>>(left, right, sel, count, true_sel,
                                                                                    false_sel, false_count);
      }
      throw NotImplementedException(
          fmt::format("comparison: cannot compare STRING against {}", LogicalType::NameOf(right.GetLogicalTypeId())));
    }
    case LogicalTypeId::DATE: {
      if (right.GetLogicalTypeId() == LogicalTypeId::STRING) {
        // The operands are swapped, so the functor inverts them back.
        return BinaryExecution::Select<string_t, date_t, StringDateCast<OP, true>>(right, left, sel, count, true_sel,
                                                                                   false_sel, false_count);
      }
      throw NotImplementedException(
          fmt::format("comparison: cannot compare DATE against {}", LogicalType::NameOf(right.GetLogicalTypeId())));
    }
    case LogicalTypeId::DECIMAL:
      return TemplatedSelectOperationDecimal<OP>(left, right, sel, count, true_sel, false_sel, false_count);
    default:
      throw NotImplementedException(
          fmt::format("comparison: unsupported left type {}", LogicalType::NameOf(left.GetLogicalTypeId())));
  }
}

/** @return True if the vector holds a nested (LIST / ARRAY) value. */
static inline auto IsNested(const Vector &vector) -> bool {
  return vector.GetType() == PhysicalType::LIST || vector.GetType() == PhysicalType::ARRAY;
}

/**
 * @brief Compare two LIST / ARRAY vectors, element by element.
 *
 * Only `=` and `<>` are defined: there is no ordering on a nested value to invent, so every
 * other comparison throws.
 *
 * Two rows are equal when they have the same length and every pair of elements is equal.
 * A NULL element equals a NULL element — that is IS NOT DISTINCT FROM applied INSIDE the
 * list, and it is what lets [1, NULL] be told apart from [1, 2] without the whole row
 * collapsing to UNKNOWN. (DuckDB yields NULL for that comparison instead; we are stricter,
 * and deliberately so, because a definite answer is what the engine's own consumers —
 * dedup, GROUP BY — need. It differs from SQL only for a list that CONTAINS a NULL.)
 *
 * A NULL ROW, on the other hand, follows the usual rule of this file: the comparison is
 * UNKNOWN, which is never TRUE, so the row lands in `false_sel`. NotDistinctFrom is built
 * on top of Equals and adds the both-NULL rows back, exactly as it does for a scalar.
 */
template <class OP>
static auto SelectNestedComparison(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                   SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  constexpr bool IS_EQUALS = std::is_same_v<OP, Equals>;
  constexpr bool IS_NOT_EQUALS = std::is_same_v<OP, NotEquals>;
  if constexpr (!IS_EQUALS && !IS_NOT_EQUALS) {
    throw NotImplementedException(fmt::format("comparison: a {} value has no ordering; only = and <> are supported",
                                              LogicalType::NameOf(left.GetType())));
  } else {
    if (left.GetLogicalType() != right.GetLogicalType()) {
      throw NotImplementedException(fmt::format("comparison: cannot compare {} against {}",
                                                left.GetLogicalType().ToString(), right.GetLogicalType().ToString()));
    }
    if (sel == nullptr) {
      sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
    }
    idx_t true_count = 0;
    false_count = 0;
    for (idx_t i = 0; i < count; i++) {
      auto idx = sel->GetIndex(i);
      bool match = false;
      if (left.RowIsValid(idx) && right.RowIsValid(idx)) {
        // Value::operator== on a LIST / ARRAY compares the elements pairwise, recursing
        // into a nested list and treating two NULL elements as equal.
        const bool equal = left.GetValue(idx) == right.GetValue(idx);
        match = IS_EQUALS ? equal : !equal;
      }
      if (match) {
        if (true_sel != nullptr) {
          true_sel->SetIndex(true_count, idx);
        }
        true_count++;
      } else {
        if (false_sel != nullptr) {
          false_sel->SetIndex(false_count, idx);
        }
        false_count++;
      }
    }
    return true_count;
  }
}

/** @brief Dispatch a comparison: the equal-type fast path, or the promoting slow path. */
template <class OP>
static auto SelectOperation(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                            SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  if (IsNested(left) || IsNested(right)) {
    return SelectNestedComparison<OP>(left, right, sel, count, true_sel, false_sel, false_count);
  }
  // Two DECIMALs of different scales share a physical type but are NOT comparable raw:
  // 1.00 (scale 2, raw 100) and 1.0 (scale 1, raw 10) would compare unequal. Send them
  // down the DECIMAL path, which brings them onto a common scale first.
  const bool both_decimal =
      left.GetLogicalTypeId() == LogicalTypeId::DECIMAL && right.GetLogicalTypeId() == LogicalTypeId::DECIMAL;
  if (left.GetType() == right.GetType() && (!both_decimal || left.GetLogicalType() == right.GetLogicalType())) {
    return TemplatedSelectOperationSwitchEqualType<OP>(left, right, sel, count, true_sel, false_sel, false_count);
  }
  return TemplatedSelectOperationSwitchLeft<OP>(left, right, sel, count, true_sel, false_sel, false_count);
}

// -- The 5-argument overloads: only the matching rows ------------------------

auto VectorOperations::Equals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                              SelectionVector *true_sel) -> idx_t {
  idx_t false_count = 0;
  return SelectOperation<class Equals>(left, right, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::NotEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                 SelectionVector *true_sel) -> idx_t {
  idx_t false_count = 0;
  return SelectOperation<class NotEquals>(left, right, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::GreaterThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                   SelectionVector *true_sel) -> idx_t {
  idx_t false_count = 0;
  return SelectOperation<class GreaterThan>(left, right, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::GreaterThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                         SelectionVector *true_sel) -> idx_t {
  idx_t false_count = 0;
  return SelectOperation<class GreaterThanEquals>(left, right, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::LessThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel) -> idx_t {
  idx_t false_count = 0;
  return SelectOperation<class LessThan>(left, right, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::LessThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                      SelectionVector *true_sel) -> idx_t {
  idx_t false_count = 0;
  return SelectOperation<class LessThanEquals>(left, right, sel, count, true_sel, nullptr, false_count);
}

// -- The 7-argument overloads: the matching AND the non-matching rows --------

auto VectorOperations::Equals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                              SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  return SelectOperation<class Equals>(left, right, sel, count, true_sel, false_sel, false_count);
}

auto VectorOperations::NotEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                 SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  return SelectOperation<class NotEquals>(left, right, sel, count, true_sel, false_sel, false_count);
}

auto VectorOperations::GreaterThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                   SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  return SelectOperation<class GreaterThan>(left, right, sel, count, true_sel, false_sel, false_count);
}

auto VectorOperations::GreaterThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                         SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count)
    -> idx_t {
  return SelectOperation<class GreaterThanEquals>(left, right, sel, count, true_sel, false_sel, false_count);
}

auto VectorOperations::LessThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  return SelectOperation<class LessThan>(left, right, sel, count, true_sel, false_sel, false_count);
}

auto VectorOperations::LessThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                      SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count)
    -> idx_t {
  return SelectOperation<class LessThanEquals>(left, right, sel, count, true_sel, false_sel, false_count);
}

// -- IS [NOT] DISTINCT FROM -------------------------------------------------

/**
 * @brief The shared core of IS [NOT] DISTINCT FROM.
 *
 * Reuses the NULL-excluding Equals to split the rows into matched (both non-NULL and
 * equal) and unmatched (everything else, which mixes the both-NULL rows in with the plain
 * value mismatches). The both-NULL rows are then added back on top (NOT DISTINCT) or
 * dropped (DISTINCT).
 */
template <bool NOT_DISTINCT>
static auto SelectDistinctOrNot(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel) -> idx_t {
  if (sel == nullptr) {
    sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
  }
  SelectionVector inner_true(count);
  SelectionVector inner_false(count);
  idx_t false_count = 0;
  idx_t matched = VectorOperations::Equals(left, right, sel, count, &inner_true, &inner_false, false_count);

  idx_t out = 0;
  if constexpr (NOT_DISTINCT) {
    if (true_sel != nullptr) {
      for (idx_t i = 0; i < matched; i++) {
        true_sel->SetIndex(out + i, inner_true.GetIndex(i));
      }
    }
    out += matched;
  }
  for (idx_t i = 0; i < false_count; i++) {
    auto idx = inner_false.GetIndex(i);
    bool both_null = !left.RowIsValid(idx) && !right.RowIsValid(idx);
    bool include = NOT_DISTINCT ? both_null : !both_null;
    if (include) {
      if (true_sel != nullptr) {
        true_sel->SetIndex(out, idx);
      }
      out++;
    }
  }
  return out;
}

auto VectorOperations::NotDistinctFrom(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                       SelectionVector *true_sel) -> idx_t {
  return SelectDistinctOrNot<true>(left, right, sel, count, true_sel);
}

auto VectorOperations::DistinctFrom(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                    SelectionVector *true_sel) -> idx_t {
  return SelectDistinctOrNot<false>(left, right, sel, count, true_sel);
}

// -- IS NULL / IS NOT NULL --------------------------------------------------

/**
 * @brief Select rows by validity alone.
 *
 * Vector::RowIsValid already handles every encoding — a constant reads bit 0, a dictionary
 * reads through the selection, a sequence is always valid — so the loop stays uniform.
 */
template <bool WANT_VALID>
static auto SelectByValidity(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                             SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  if (sel == nullptr) {
    sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
  }
  idx_t true_count = 0;
  false_count = 0;
  for (idx_t i = 0; i < count; i++) {
    auto idx = sel->GetIndex(i);
    bool match = input.RowIsValid(idx) == WANT_VALID;
    if (match) {
      if (true_sel != nullptr) {
        true_sel->SetIndex(true_count, idx);
      }
      true_count++;
    } else {
      if (false_sel != nullptr) {
        false_sel->SetIndex(false_count, idx);
      }
      false_count++;
    }
  }
  return true_count;
}

auto VectorOperations::IsNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel)
    -> idx_t {
  idx_t false_count = 0;
  return SelectByValidity<false>(input, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::IsNotNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel)
    -> idx_t {
  idx_t false_count = 0;
  return SelectByValidity<true>(input, sel, count, true_sel, nullptr, false_count);
}

auto VectorOperations::IsNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                              SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  return SelectByValidity<false>(input, sel, count, true_sel, false_sel, false_count);
}

auto VectorOperations::IsNotNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                                 SelectionVector *false_sel, idx_t &false_count) -> idx_t {
  return SelectByValidity<true>(input, sel, count, true_sel, false_sel, false_count);
}

}  // namespace bumblebee
