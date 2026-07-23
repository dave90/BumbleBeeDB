//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// expression_executor.cpp
//
// Identification: src/execution/expression_executor.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/expression_executor.h"

#include <cctype>
#include <cstring>
#include <string>
#include <utility>

#include "common/exception.h"
#include "execution/expressions/arithmetic_expression.h"
#include "execution/expressions/cast_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/is_null_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/expressions/string_expression.h"
#include "type/value.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

namespace {

/** @brief Cast `v` to `target` if its physical type differs; otherwise move it through untouched. */
auto CastIfNeeded(Vector v, const LogicalType &target, idx_t count) -> Vector {
  if (v.GetLogicalType().GetPhysicalType() == target.GetPhysicalType()) {
    return v;
  }
  Vector out(target);
  VectorOperations::Cast(v, out, count);
  return out;
}

/** @brief Run the comparison `t` over `left`/`right`, filling `true_sel` with matching rows. */
auto RunCompare(ComparisonType t, Vector &left, Vector &right, idx_t count, SelectionVector *true_sel) -> idx_t {
  switch (t) {
    case ComparisonType::Equal:
      return VectorOperations::Equals(left, right, nullptr, count, true_sel);
    case ComparisonType::NotEqual:
      return VectorOperations::NotEquals(left, right, nullptr, count, true_sel);
    case ComparisonType::LessThan:
      return VectorOperations::LessThan(left, right, nullptr, count, true_sel);
    case ComparisonType::LessThanOrEqual:
      return VectorOperations::LessThanEquals(left, right, nullptr, count, true_sel);
    case ComparisonType::GreaterThan:
      return VectorOperations::GreaterThan(left, right, nullptr, count, true_sel);
    case ComparisonType::GreaterThanOrEqual:
      return VectorOperations::GreaterThanEquals(left, right, nullptr, count, true_sel);
  }
  throw NotImplementedException("ExpressionExecutor: unsupported comparison type");
}

/** @brief A freshly allocated FLAT boolean (UTINYINT) vector of `count` rows set to 0/1 from `true_sel`. */
auto BoolVectorFromSelection(const SelectionVector &true_sel, idx_t true_count, idx_t count) -> Vector {
  Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
  auto *data = FlatVector::GetData<uint8_t>(result);
  std::memset(data, 0, count * sizeof(uint8_t));
  for (idx_t i = 0; i < true_count; i++) {
    data[true_sel.GetIndex(i)] = 1;
  }
  return result;
}

/** @brief Run the arithmetic op `t` over `left`/`right` (both already at the result type) into `result`. */
void RunArithmetic(ArithmeticType t, Vector &left, Vector &right, Vector &result, idx_t count) {
  switch (t) {
    case ArithmeticType::Plus:
      VectorOperations::Sum(left, right, result, count);
      return;
    case ArithmeticType::Minus:
      VectorOperations::Difference(left, right, result, count);
      return;
    case ArithmeticType::Multiply:
      VectorOperations::Dot(left, right, result, count);
      return;
    case ArithmeticType::Divide:
      VectorOperations::Division(left, right, result, count);
      return;
  }
  throw NotImplementedException("ExpressionExecutor: unsupported arithmetic type");
}

/**
 * @brief Combine two boolean vectors with AND/OR into a fresh boolean vector.
 *
 * The AND/OR choice is hoisted out of the row loop so each variant is a tight, branchless pass the
 * compiler can auto-vectorize. When neither side carries a validity mask (the common case — NULLs are
 * folded to false by the comparisons that feed this) the loop is a pure bitwise combine over two
 * contiguous byte arrays; otherwise it falls back to a validity-aware per-row combine.
 */
auto RunLogic(LogicType t, Vector &left, Vector &right, idx_t count) -> Vector {
  left.Normalify(count);
  right.Normalify(count);
  const auto *ld = FlatVector::GetData<uint8_t>(left);
  const auto *rd = FlatVector::GetData<uint8_t>(right);
  Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
  auto *out = FlatVector::GetData<uint8_t>(result);

  if (left.Validity().AllValid() && right.Validity().AllValid()) {
    // Fast path: no NULLs on either side, so a contiguous branchless bitwise combine — vectorizable and
    // cache-friendly (both inputs and the output are swept linearly, once).
    if (t == LogicType::And) {
      for (idx_t i = 0; i < count; i++) {
        out[i] = static_cast<uint8_t>((ld[i] != 0) & (rd[i] != 0));
      }
    } else {
      for (idx_t i = 0; i < count; i++) {
        out[i] = static_cast<uint8_t>((ld[i] != 0) | (rd[i] != 0));
      }
    }
    return result;
  }

  // Slow path: fold each side's validity into its truth value (a NULL reads as false).
  if (t == LogicType::And) {
    for (idx_t i = 0; i < count; i++) {
      out[i] = static_cast<uint8_t>((left.RowIsValid(i) && ld[i] != 0) && (right.RowIsValid(i) && rd[i] != 0));
    }
  } else {
    for (idx_t i = 0; i < count; i++) {
      out[i] = static_cast<uint8_t>((left.RowIsValid(i) && ld[i] != 0) || (right.RowIsValid(i) && rd[i] != 0));
    }
  }
  return result;
}

/** @brief Apply the string transform `t` (upper/lower) per row through Value; NULLs pass through. */
auto RunString(StringExpressionType t, Vector &arg, idx_t count) -> Vector {
  arg.Normalify(count);
  Vector result{LogicalType(LogicalTypeId::STRING)};

  // Select the per-character transform once, from the expression type.
  bool to_upper = false;
  switch (t) {
    case StringExpressionType::Upper:
      to_upper = true;
      break;
    case StringExpressionType::Lower:
      to_upper = false;
      break;
  }

  for (idx_t i = 0; i < count; i++) {
    Value v = arg.GetValue(i);
    if (v.IsNull()) {
      result.SetValue(i, Value::Null(LogicalType(LogicalTypeId::STRING)));
      continue;
    }
    std::string s = v.GetString();
    for (auto &ch : s) {
      const auto uch = static_cast<unsigned char>(ch);
      ch = to_upper ? static_cast<char>(std::toupper(uch)) : static_cast<char>(std::tolower(uch));
    }
    result.SetValue(i, Value(std::move(s)));
  }
  return result;
}

}  // namespace

auto ExpressionExecutor::Evaluate(const AbstractExpression &expr, DataChunk &input, idx_t count) -> Vector {
  // Column reference: zero-copy view onto the input column.
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(&expr)) {
    return Vector(input.data_[col->GetColIdx()]);
  }

  // Constant: a CONSTANT_VECTOR broadcasting the literal.
  if (const auto *cst = dynamic_cast<const ConstantValueExpression *>(&expr)) {
    return Vector(cst->val_);
  }

  // Arithmetic: evaluate both sides, cast to the result type, run the vectorized kernel.
  if (const auto *arith = dynamic_cast<const ArithmeticExpression *>(&expr)) {
    const LogicalType rtype = arith->GetReturnType().GetType();
    Vector left = CastIfNeeded(Evaluate(*arith->children_[0], input, count), rtype, count);
    Vector right = CastIfNeeded(Evaluate(*arith->children_[1], input, count), rtype, count);
    Vector result(rtype);
    RunArithmetic(arith->compute_type_, left, right, result, count);
    return result;
  }

  // Cast: evaluate the child, then materialize it at the target type (no-op when already that type).
  if (const auto *cast = dynamic_cast<const CastExpression *>(&expr)) {
    Vector child = Evaluate(*cast->children_[0], input, count);
    const auto target = cast->GetReturnType().GetType();
    if (!cast->strict_) {
      return CastIfNeeded(std::move(child), target, count);
    }
    // Explicit SQL CAST: run the try-cast kernels and ERROR on a failed row (the implicit path
    // above silently NULLs it). Same-type casts still pass through, but the result always
    // carries the TARGET logical type (e.g. BIGINT -> TIMESTAMP shares a physical int64).
    if (child.GetLogicalType() == target) {
      return child;
    }
    Vector out(target);
    std::string error;
    if (!VectorOperations::TryCast(child, out, count, &error)) {
      throw ExecutionException(fmt::format("CAST to {} failed: {}", target.ToString(),
                                           error.empty() ? "value out of range or malformed" : error));
    }
    return out;
  }

  // IS [NOT] NULL: a boolean read straight off the child's validity mask — the one predicate that
  // can select NULL rows (a comparison against NULL yields NULL and matches nothing).
  if (const auto *is_null = dynamic_cast<const IsNullExpression *>(&expr)) {
    Vector child = Evaluate(*is_null->children_[0], input, count);
    VectorData vdata;
    child.Orrify(count, vdata);
    Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
    auto *out = FlatVector::GetData<uint8_t>(result);
    for (idx_t i = 0; i < count; i++) {
      const auto row = vdata.sel_->GetIndex(i);
      const bool row_is_null = !vdata.validity_->RowIsValid(row);
      out[i] = static_cast<uint8_t>(row_is_null != is_null->negated_ ? 1 : 0);
    }
    return result;
  }

  // Comparison: evaluate both sides at a common type, produce a boolean (0/1) vector.
  if (const auto *cmp = dynamic_cast<const ComparisonExpression *>(&expr)) {
    Vector left0 = Evaluate(*cmp->children_[0], input, count);
    Vector right0 = Evaluate(*cmp->children_[1], input, count);
    const LogicalType common = LogicalType::CommonType(left0.GetLogicalType(), right0.GetLogicalType());
    Vector left = CastIfNeeded(std::move(left0), common, count);
    Vector right = CastIfNeeded(std::move(right0), common, count);
    SelectionVector true_sel(count == 0 ? 1 : count);
    idx_t true_count = RunCompare(cmp->comp_type_, left, right, count, &true_sel);
    return BoolVectorFromSelection(true_sel, true_count, count);
  }

  // Logic: both sides are boolean vectors; combine element-wise (NULLs already folded to 0/false).
  if (const auto *logic = dynamic_cast<const LogicExpression *>(&expr)) {
    Vector left = Evaluate(*logic->children_[0], input, count);
    Vector right = Evaluate(*logic->children_[1], input, count);
    return RunLogic(logic->logic_type_, left, right, count);
  }

  // String upper/lower: no vector kernel yet, so a correct per-row fallback through Value.
  if (const auto *str = dynamic_cast<const StringExpression *>(&expr)) {
    Vector arg = Evaluate(*str->children_[0], input, count);
    return RunString(str->expr_type_, arg, count);
  }

  throw NotImplementedException("ExpressionExecutor: unsupported expression type");
}

void ExpressionExecutor::ExecuteExpression(idx_t expr_idx, DataChunk &input, Vector &result) {
  Vector v = Evaluate(*roots_[expr_idx], input, input.GetSize());
  result.Reference(v);
}

void ExpressionExecutor::Execute(DataChunk &input, DataChunk &result) {
  const idx_t count = input.GetSize();
  for (idx_t i = 0; i < roots_.size(); i++) {
    Vector v = Evaluate(*roots_[i], input, count);
    result.data_[i].Reference(v);  // shares the data manager; v may safely go out of scope
  }
  result.SetCardinality(count);
}

auto ExpressionExecutor::Select(DataChunk &input, SelectionVector &sel) -> idx_t {
  const idx_t count = input.GetSize();
  Vector b = Evaluate(*roots_[0], input, count);
  b.Normalify(count);
  const auto *data = FlatVector::GetData<uint8_t>(b);
  idx_t n = 0;
  for (idx_t i = 0; i < count; i++) {
    if (b.RowIsValid(i) && data[i] != 0) {
      sel.SetIndex(n++, i);
    }
  }
  return n;
}

}  // namespace bumblebee
