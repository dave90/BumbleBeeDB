//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_zone_filter.cpp
//
// Identification: src/storage/parquet/parquet_zone_filter.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_zone_filter.h"

#include <cstring>
#include <optional>

#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"

namespace bumblebee {

namespace {

auto FlipComparison(ComparisonType t) -> ComparisonType {
  switch (t) {
    case ComparisonType::LessThan:
      return ComparisonType::GreaterThan;
    case ComparisonType::LessThanOrEqual:
      return ComparisonType::GreaterThanOrEqual;
    case ComparisonType::GreaterThan:
      return ComparisonType::LessThan;
    case ComparisonType::GreaterThanOrEqual:
      return ComparisonType::LessThanOrEqual;
    default:
      return t;  // Equal / NotEqual are symmetric
  }
}

/** The numeric [min, max] of one column chunk, in double domain (exact for int48-scale values,
 * and a conservative approximation beyond — good enough for pruning). */
struct NumericRange {
  double min_;
  double max_;
};

/** @brief Decode a plain-encoded statistics payload of the chunk's parquet physical type. */
auto DecodeNumericStat(const std::string &bytes, format::Type::type type) -> std::optional<double> {
  switch (type) {
    case format::Type::INT32: {
      if (bytes.size() != sizeof(int32_t)) {
        return std::nullopt;
      }
      int32_t v;
      std::memcpy(&v, bytes.data(), sizeof(v));
      return static_cast<double>(v);
    }
    case format::Type::INT64: {
      if (bytes.size() != sizeof(int64_t)) {
        return std::nullopt;
      }
      int64_t v;
      std::memcpy(&v, bytes.data(), sizeof(v));
      return static_cast<double>(v);
    }
    case format::Type::FLOAT: {
      if (bytes.size() != sizeof(float)) {
        return std::nullopt;
      }
      float v;
      std::memcpy(&v, bytes.data(), sizeof(v));
      return static_cast<double>(v);
    }
    case format::Type::DOUBLE: {
      if (bytes.size() != sizeof(double)) {
        return std::nullopt;
      }
      double v;
      std::memcpy(&v, bytes.data(), sizeof(v));
      return v;
    }
    default:
      return std::nullopt;
  }
}

/** @brief The numeric range of a column chunk, when trustworthy statistics exist. */
auto NumericStatsOf(const format::ColumnChunk &chunk) -> std::optional<NumericRange> {
  if (!chunk.__isset.meta_data || !chunk.meta_data.__isset.statistics) {
    return std::nullopt;
  }
  const auto &meta = chunk.meta_data;
  const auto &stats = meta.statistics;
  const std::string *min_bytes = nullptr;
  const std::string *max_bytes = nullptr;
  if (stats.__isset.min_value && stats.__isset.max_value) {
    min_bytes = &stats.min_value;
    max_bytes = &stats.max_value;
  } else if (stats.__isset.min && stats.__isset.max) {
    // Legacy fields: their sort order is only unambiguous for signed numerics — exactly the types
    // DecodeNumericStat accepts.
    min_bytes = &stats.min;
    max_bytes = &stats.max;
  } else {
    return std::nullopt;
  }
  auto min_v = DecodeNumericStat(*min_bytes, meta.type);
  auto max_v = DecodeNumericStat(*max_bytes, meta.type);
  if (!min_v.has_value() || !max_v.has_value()) {
    return std::nullopt;
  }
  return NumericRange{*min_v, *max_v};
}

/** @brief Plain numeric logical type (no domain shift: excludes DECIMAL / DATE / TIMESTAMP). */
auto IsPlainNumericTypeId(LogicalTypeId id) -> bool {
  switch (id) {
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
      return true;
    default:
      return false;
  }
}

}  // namespace

void ExtractZonePredicates(const AbstractExpressionRef &expr, std::vector<ZonePredicate> &out) {
  if (expr == nullptr) {
    return;
  }
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expr.get())) {
    if (logic->logic_type_ == LogicType::And) {
      ExtractZonePredicates(logic->GetChildAt(0), out);
      ExtractZonePredicates(logic->GetChildAt(1), out);
    }
    return;  // OR branches cannot be used one-sidedly
  }
  const auto *cmp = dynamic_cast<const ComparisonExpression *>(expr.get());
  if (cmp == nullptr || cmp->comp_type_ == ComparisonType::NotEqual) {
    return;
  }
  const auto *lhs_col = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(0).get());
  const auto *rhs_col = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(1).get());
  const auto *lhs_const = dynamic_cast<const ConstantValueExpression *>(cmp->GetChildAt(0).get());
  const auto *rhs_const = dynamic_cast<const ConstantValueExpression *>(cmp->GetChildAt(1).get());

  if (lhs_col != nullptr && rhs_const != nullptr && !rhs_const->val_.IsNull()) {
    out.push_back(ZonePredicate{lhs_col->GetColIdx(), cmp->comp_type_, rhs_const->val_});
  } else if (lhs_const != nullptr && rhs_col != nullptr && !lhs_const->val_.IsNull()) {
    out.push_back(ZonePredicate{rhs_col->GetColIdx(), FlipComparison(cmp->comp_type_), lhs_const->val_});
  }
}

auto RowGroupCanMatch(const format::RowGroup &group, const std::vector<LogicalType> &column_types,
                      const std::vector<ZonePredicate> &predicates) -> bool {
  for (const auto &p : predicates) {
    if (p.column_ >= group.columns.size() || p.column_ >= column_types.size()) {
      continue;
    }
    // Both the column's LOGICAL type and the constant must be plain numerics: a DECIMAL / DATE /
    // TIMESTAMP column stores domain-shifted payloads whose raw statistics must not be compared
    // against a SQL constant. (The column chunk metadata alone cannot tell these apart — only
    // the schema-derived logical type can.)
    if (!IsPlainNumericTypeId(column_types[p.column_].GetTypeId()) ||
        !IsPlainNumericTypeId(p.constant_.GetType().GetTypeId())) {
      continue;
    }
    const auto &chunk = group.columns[p.column_];
    if (!chunk.__isset.meta_data) {
      continue;
    }
    auto range = NumericStatsOf(chunk);
    if (!range.has_value()) {
      continue;
    }
    const double c = p.constant_.GetAs<double>();
    bool can_match = true;
    switch (p.op_) {
      case ComparisonType::Equal:
        can_match = c >= range->min_ && c <= range->max_;
        break;
      case ComparisonType::LessThan:
        can_match = range->min_ < c;
        break;
      case ComparisonType::LessThanOrEqual:
        can_match = range->min_ <= c;
        break;
      case ComparisonType::GreaterThan:
        can_match = range->max_ > c;
        break;
      case ComparisonType::GreaterThanOrEqual:
        can_match = range->max_ >= c;
        break;
      default:
        break;
    }
    if (!can_match) {
      return false;  // this predicate alone proves the group empty
    }
  }
  return true;
}

}  // namespace bumblebee
