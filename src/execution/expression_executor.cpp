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
#include "execution/expressions/in_expression.h"
#include "execution/expressions/is_null_expression.h"
#include "execution/expressions/like_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/expressions/string_expression.h"
#include "type/value.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/** @brief Cast `v` to `target` if its physical type differs; otherwise move it through untouched. */
static auto CastIfNeeded(Vector v, const LogicalType &target, idx_t count) -> Vector {
  if (v.GetLogicalType().GetPhysicalType() == target.GetPhysicalType()) {
    return v;
  }
  Vector out(target);
  VectorOperations::Cast(v, out, count);
  return out;
}

/** @brief Run the comparison `t` over `left`/`right`, filling `true_sel` with matching rows.
 * `sel` restricts the tested rows (nullptr = all of [0, count)); the emitted entries are original
 * row indexes, so successive comparisons can chain through their predecessors' output. */
static auto RunCompare(ComparisonType t, Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                       SelectionVector *true_sel) -> idx_t {
  switch (t) {
    case ComparisonType::Equal:
      return VectorOperations::Equals(left, right, sel, count, true_sel);
    case ComparisonType::NotEqual:
      return VectorOperations::NotEquals(left, right, sel, count, true_sel);
    case ComparisonType::LessThan:
      return VectorOperations::LessThan(left, right, sel, count, true_sel);
    case ComparisonType::LessThanOrEqual:
      return VectorOperations::LessThanEquals(left, right, sel, count, true_sel);
    case ComparisonType::GreaterThan:
      return VectorOperations::GreaterThan(left, right, sel, count, true_sel);
    case ComparisonType::GreaterThanOrEqual:
      return VectorOperations::GreaterThanEquals(left, right, sel, count, true_sel);
  }
  throw NotImplementedException("ExpressionExecutor: unsupported comparison type");
}

/** @brief Flatten an AND tree into comparison leaves. False iff any leaf is not a comparison. */
static auto CollectComparisonConjuncts(const AbstractExpression &expr, std::vector<const ComparisonExpression *> &out)
    -> bool {
  if (const auto *cmp = dynamic_cast<const ComparisonExpression *>(&expr)) {
    out.push_back(cmp);
    return true;
  }
  if (const auto *logic = dynamic_cast<const LogicExpression *>(&expr);
      logic != nullptr && logic->logic_type_ == LogicType::And) {
    return CollectComparisonConjuncts(*logic->children_[0], out) &&
           CollectComparisonConjuncts(*logic->children_[1], out);
  }
  return false;
}

/** @brief A freshly allocated FLAT boolean (UTINYINT) vector of `count` rows set to 0/1 from `true_sel`. */
static auto BoolVectorFromSelection(const SelectionVector &true_sel, idx_t true_count, idx_t count) -> Vector {
  Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
  auto *data = FlatVector::GetData<uint8_t>(result);
  std::memset(data, 0, count * sizeof(uint8_t));
  for (idx_t i = 0; i < true_count; i++) {
    data[true_sel.GetIndex(i)] = 1;
  }
  return result;
}

/** @brief Run the arithmetic op `t` over `left`/`right` (both already at the result type) into `result`. */
static void RunArithmetic(ArithmeticType t, Vector &left, Vector &right, Vector &result, idx_t count) {
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
static auto RunLogic(LogicType t, Vector &left, Vector &right, idx_t count) -> Vector {
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
static auto RunString(StringExpressionType t, Vector &arg, idx_t count) -> Vector {
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

/** @brief SQL LIKE match: `%` matches any run of bytes (incl. none), `_` any single byte, the rest
 * literally. Iterative with `%` backtracking — O(slen * plen) worst case, no allocation. */
static auto LikeMatch(const char *s, size_t slen, const char *p, size_t plen) -> bool {
  size_t si = 0;
  size_t pi = 0;
  size_t star_pi = std::string::npos;  // last '%' position in the pattern, for backtracking
  size_t star_si = 0;                  // where in the string that '%' started matching
  while (si < slen) {
    // '%' must be dispatched as a wildcard BEFORE the literal-byte compare: a literal '%' in the
    // data aligned with a pattern '%' would otherwise satisfy `p[pi] == s[si]` and be consumed as
    // a literal match, never registering the wildcard (the string is full of '%' escapes in URLs).
    if (pi < plen && p[pi] == '%') {
      star_pi = pi++;
      star_si = si;
    } else if (pi < plen && (p[pi] == '_' || p[pi] == s[si])) {
      si++;
      pi++;
    } else if (star_pi != std::string::npos) {
      pi = star_pi + 1;  // the last '%' absorbs one more string byte and we retry
      si = ++star_si;
    } else {
      return false;
    }
  }
  while (pi < plen && p[pi] == '%') {
    pi++;  // trailing '%'s match the empty remainder
  }
  return pi == plen;
}

/** @brief Evaluate `input [NOT] LIKE pattern` -> BOOLEAN. Reads both string operands through
 * Orrify so any encoding (flat, dictionary, constant, sequence) is handled by one loop; a NULL
 * operand yields false (never matches in a WHERE, mirroring how a NULL comparison selects nothing). */
static auto RunLike(bool negated, Vector &input, Vector &pattern, idx_t count) -> Vector {
  VectorData in;
  VectorData pat;
  input.Orrify(count, in);
  pattern.Orrify(count, pat);
  const auto *in_strs = reinterpret_cast<const string_t *>(in.data_);
  const auto *pat_strs = reinterpret_cast<const string_t *>(pat.data_);

  Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
  auto *out = FlatVector::GetData<uint8_t>(result);
  for (idx_t i = 0; i < count; i++) {
    const auto irow = in.sel_->GetIndex(i);
    const auto prow = pat.sel_->GetIndex(i);
    if (!in.validity_->RowIsValid(irow) || !pat.validity_->RowIsValid(prow)) {
      out[i] = 0;
      continue;
    }
    const auto &s = in_strs[irow];
    const auto &p = pat_strs[prow];
    const bool matched = LikeMatch(s.GetDataUnsafe(), s.Size(), p.GetDataUnsafe(), p.Size());
    out[i] = static_cast<uint8_t>((matched != negated) ? 1 : 0);
  }
  return result;
}

/** @brief True for the integer-backed physical types, whose `<` ordering agrees with `==` — so a
 * sorted list can answer membership by binary search. Floats are excluded (NaN has no total order)
 * and so are the variable-length types. DATE/TIMESTAMP/DECIMAL ride along on their integer
 * representation, which the cast to the comparison's common type has already normalized. */
static auto IsBinarySearchable(PhysicalType ptype) -> bool {
  switch (ptype) {
    case PhysicalType::TINYINT:
    case PhysicalType::SMALLINT:
    case PhysicalType::INTEGER:
    case PhysicalType::BIGINT:
    case PhysicalType::UTINYINT:
    case PhysicalType::USMALLINT:
    case PhysicalType::UINTEGER:
    case PhysicalType::UBIGINT:
      return true;
    default:
      return false;
  }
}

/** @brief Sort the raw `count` values of type T held in `bytes`. */
template <class T>
static void SortRawValues(std::vector<uint8_t> &bytes, idx_t count) {
  auto *values = reinterpret_cast<T *>(bytes.data());
  std::sort(values, values + count);
}

/** @brief `tested [NOT] IN (<sorted constants>)`, one binary search per row. */
template <class T>
static auto ProbeSortedIn(bool negated, VectorData &tv, const uint8_t *bytes, idx_t list_count, bool list_has_null,
                          idx_t count) -> Vector {
  const auto *values = reinterpret_cast<const T *>(bytes);
  const auto *end = values + list_count;
  const auto *data = reinterpret_cast<const T *>(tv.data_);

  Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
  auto *out = FlatVector::GetData<uint8_t>(result);
  for (idx_t i = 0; i < count; i++) {
    const idx_t idx = tv.sel_->GetIndex(i);
    if (!tv.validity_->RowIsValid(idx)) {
      out[i] = 0;  // NULL on the tested side: NULL for both polarities, which folds to 0
      continue;
    }
    const bool matched = std::binary_search(values, end, data[idx]);
    // A non-match against a list holding a NULL is NULL, not false — so NOT IN emits 0 there too.
    out[i] = static_cast<uint8_t>(negated ? (!matched && !list_has_null) : matched);
  }
  return result;
}

/**
 * @brief Evaluate `tested [NOT] IN (list...)` -> BOOLEAN, all operands already at a common type.
 *
 * SQL three-valued semantics folded to this engine's 0/1 convention: a match wins outright (IN=1,
 * NOT IN=0); a non-match that involved a NULL (the tested value or any list element) is NULL in
 * SQL, which folds to 0 for BOTH polarities — that is why NOT IN needs the `null_seen` tracking
 * and cannot be an outer NOT over the IN result.
 */
static auto RunIn(bool negated, Vector &tested, std::vector<Vector> &list, idx_t count) -> Vector {
  VectorData tv;
  tested.Orrify(count, tv);

  std::vector<uint8_t> matched(count, 0);
  std::vector<uint8_t> null_seen(count, 0);
  for (idx_t i = 0; i < count; i++) {
    null_seen[i] = static_cast<uint8_t>(!tv.validity_->RowIsValid(tv.sel_->GetIndex(i)));
  }

  SelectionVector true_sel(count == 0 ? 1 : count);
  for (auto &item : list) {
    const idx_t true_count = VectorOperations::Equals(tested, item, nullptr, count, &true_sel);
    for (idx_t j = 0; j < true_count; j++) {
      matched[true_sel.GetIndex(j)] = 1;
    }
    VectorData iv;
    item.Orrify(count, iv);
    if (!iv.validity_->AllValid()) {
      for (idx_t i = 0; i < count; i++) {
        null_seen[i] |= static_cast<uint8_t>(!iv.validity_->RowIsValid(iv.sel_->GetIndex(i)));
      }
    }
  }

  Vector result{LogicalType(LogicalTypeId::BOOLEAN)};
  auto *out = FlatVector::GetData<uint8_t>(result);
  if (negated) {
    for (idx_t i = 0; i < count; i++) {
      out[i] = static_cast<uint8_t>(matched[i] == 0 && null_seen[i] == 0);
    }
  } else {
    for (idx_t i = 0; i < count; i++) {
      out[i] = matched[i];
    }
  }
  return result;
}

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
    idx_t true_count = RunCompare(cmp->comp_type_, left, right, nullptr, count, &true_sel);
    return BoolVectorFromSelection(true_sel, true_count, count);
  }

  // Logic: both sides are boolean vectors; combine element-wise (NULLs already folded to 0/false).
  if (const auto *logic = dynamic_cast<const LogicExpression *>(&expr)) {
    Vector left = Evaluate(*logic->children_[0], input, count);
    Vector right = Evaluate(*logic->children_[1], input, count);
    return RunLogic(logic->logic_type_, left, right, count);
  }

  // LIKE / NOT LIKE: evaluate both sides to strings, then match per row -> boolean.
  if (const auto *like = dynamic_cast<const LikeExpression *>(&expr)) {
    Vector s = Evaluate(*like->children_[0], input, count);
    Vector p = Evaluate(*like->children_[1], input, count);
    return RunLike(like->negated_, s, p, count);
  }

  // [NOT] IN over a value list: evaluate every operand at a common type, then one membership pass.
  if (const auto *in = dynamic_cast<const InExpression *>(&expr)) {
    LogicalType common = in->children_[0]->GetReturnType().GetType();
    for (size_t i = 1; i < in->children_.size(); i++) {
      common = LogicalType::CommonType(common, in->children_[i]->GetReturnType().GetType());
    }
    Vector tested = CastIfNeeded(Evaluate(*in->children_[0], input, count), common, count);
    // An all-constant integer list is prepared (and sorted) once, then answered by binary search:
    // the general kernel below costs one full-chunk Equals PER list element, on every chunk.
    if (const auto &prepared = GetConstantInList(*in, common); prepared.usable_) {
      VectorData tv;
      tested.Orrify(count, tv);
      const auto *bytes = prepared.sorted_.data();
      switch (prepared.ptype_) {
        case PhysicalType::TINYINT:
          return ProbeSortedIn<int8_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        case PhysicalType::SMALLINT:
          return ProbeSortedIn<int16_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        case PhysicalType::INTEGER:
          return ProbeSortedIn<int32_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        case PhysicalType::BIGINT:
          return ProbeSortedIn<int64_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        case PhysicalType::UTINYINT:
          return ProbeSortedIn<uint8_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        case PhysicalType::USMALLINT:
          return ProbeSortedIn<uint16_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        case PhysicalType::UINTEGER:
          return ProbeSortedIn<uint32_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
        default:
          return ProbeSortedIn<uint64_t>(in->negated_, tv, bytes, prepared.count_, prepared.has_null_, count);
      }
    }
    std::vector<Vector> items;
    items.reserve(in->children_.size() - 1);
    for (size_t i = 1; i < in->children_.size(); i++) {
      items.push_back(CastIfNeeded(Evaluate(*in->children_[i], input, count), common, count));
    }
    return RunIn(in->negated_, tested, items, count);
  }

  // String upper/lower: no vector kernel yet, so a correct per-row fallback through Value.
  if (const auto *str = dynamic_cast<const StringExpression *>(&expr)) {
    Vector arg = Evaluate(*str->children_[0], input, count);
    return RunString(str->expr_type_, arg, count);
  }

  throw NotImplementedException("ExpressionExecutor: unsupported expression type");
}

auto ExpressionExecutor::GetConstantInList(const AbstractExpression &expr, const LogicalType &common)
    -> const ConstantInList & {
  auto [it, inserted] = in_lists_.try_emplace(&expr);
  auto &prepared = it->second;
  if (!inserted) {
    return prepared;
  }

  const auto ptype = common.GetPhysicalType();
  if (!IsBinarySearchable(ptype)) {
    return prepared;  // usable_ stays false: the general kernel handles it
  }
  const idx_t width = LogicalType::SizeOf(ptype);
  const auto &children = expr.children_;
  prepared.sorted_.resize((children.size() - 1) * width);
  idx_t n = 0;
  for (size_t i = 1; i < children.size(); i++) {
    const auto *constant = dynamic_cast<const ConstantValueExpression *>(children[i].get());
    if (constant == nullptr) {
      return prepared;  // a non-constant element: nothing to prepare, take the general path
    }
    if (constant->val_.IsNull()) {
      prepared.has_null_ = true;
      continue;  // a NULL never matches; it only makes a non-match unknown
    }
    // Cast through a one-row vector so the element lands in exactly the representation the general
    // kernel's Equals would have compared against.
    Vector one(constant->val_);
    Vector at_common = CastIfNeeded(std::move(one), common, 1);
    at_common.Normalify(1);
    std::memcpy(prepared.sorted_.data() + n * width, at_common.GetData(), width);
    n++;
  }
  prepared.sorted_.resize(n * width);
  prepared.count_ = n;
  prepared.ptype_ = ptype;
  prepared.usable_ = true;

  switch (ptype) {
    case PhysicalType::TINYINT:
      SortRawValues<int8_t>(prepared.sorted_, n);
      break;
    case PhysicalType::SMALLINT:
      SortRawValues<int16_t>(prepared.sorted_, n);
      break;
    case PhysicalType::INTEGER:
      SortRawValues<int32_t>(prepared.sorted_, n);
      break;
    case PhysicalType::BIGINT:
      SortRawValues<int64_t>(prepared.sorted_, n);
      break;
    case PhysicalType::UTINYINT:
      SortRawValues<uint8_t>(prepared.sorted_, n);
      break;
    case PhysicalType::USMALLINT:
      SortRawValues<uint16_t>(prepared.sorted_, n);
      break;
    case PhysicalType::UINTEGER:
      SortRawValues<uint32_t>(prepared.sorted_, n);
      break;
    default:
      SortRawValues<uint64_t>(prepared.sorted_, n);
      break;
  }
  return prepared;
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
  // Conjunctions of comparisons (including the bare single comparison — the overwhelmingly
  // common filter shape) run as a narrowing chain: each comparison kernel writes original row
  // indexes and the next one tests only those. The general path below would instead materialize
  // a boolean vector per node, AND them element-wise over EVERY row, and rescan the result per
  // row — a date-range `a >= x AND a < y` over a full scan pays that on all 100M rows.
  std::vector<const ComparisonExpression *> conjuncts;
  if (count > 0 && CollectComparisonConjuncts(*roots_[0], conjuncts)) {
    if (narrow_scratch_size_ < count) {
      narrow_scratch_.Initialize(count);
      narrow_scratch_size_ = count;
    }
    // Ping-pong so the final comparison lands in the caller's `sel`.
    SelectionVector *next = (conjuncts.size() % 2 == 1) ? &sel : &narrow_scratch_;
    const SelectionVector *cur = nullptr;
    idx_t n = count;
    for (const auto *cmp : conjuncts) {
      if (n == 0) {
        return 0;
      }
      Vector left0 = Evaluate(*cmp->children_[0], input, count);
      Vector right0 = Evaluate(*cmp->children_[1], input, count);
      const LogicalType common = LogicalType::CommonType(left0.GetLogicalType(), right0.GetLogicalType());
      Vector left = CastIfNeeded(std::move(left0), common, count);
      Vector right = CastIfNeeded(std::move(right0), common, count);
      n = RunCompare(cmp->comp_type_, left, right, cur, n, next);
      cur = next;
      next = (next == &sel) ? &narrow_scratch_ : &sel;
    }
    BUMBLEBEE_ASSERT(cur == &sel, "the narrowing chain must end in the caller's selection");
    return n;
  }
  Vector b = Evaluate(*roots_[0], input, count);
  b.Normalify(count);
  const auto *data = FlatVector::GetData<uint8_t>(b);
  const auto &validity = b.Validity();
  idx_t n = 0;
  if (validity.AllValid()) {
    for (idx_t i = 0; i < count; i++) {
      if (data[i] != 0) {
        sel.SetIndex(n++, i);
      }
    }
  } else {
    for (idx_t i = 0; i < count; i++) {
      if (validity.RowIsValid(i) && data[i] != 0) {
        sel.SetIndex(n++, i);
      }
    }
  }
  return n;
}

}  // namespace bumblebee
