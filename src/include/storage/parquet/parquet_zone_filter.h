//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_zone_filter.h
//
// Identification: src/include/storage/parquet/parquet_zone_filter.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

#include "execution/expressions/abstract_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "parquet/parquet_types.h"
#include "type/value.h"

namespace bumblebee {

/** One provable `column <op> constant` conjunct usable against row-group min/max statistics. */
struct ZonePredicate {
  idx_t column_;        // table (= file) column index
  ComparisonType op_;   // never NotEqual (a zone map cannot prune it)
  Value constant_;
};

/**
 * @brief Extract the zone-map-usable conjuncts of a scan predicate.
 *
 * Walks AND chains; each `col <op> const` / `const <op> col` (op flipped) comparison becomes a
 * ZonePredicate. Anything else — ORs, expressions over the column, NULL checks — contributes
 * nothing (the streaming filter above the scan still applies the FULL predicate; zone pruning is
 * only ever an optimization, so under-extracting is always safe).
 */
void ExtractZonePredicates(const AbstractExpressionRef &expr, std::vector<ZonePredicate> &out);

/**
 * @brief Can `group` contain rows satisfying every predicate?
 *
 * Compares each predicate against the row group's column statistics (min_value/max_value; the
 * legacy min/max fields only for numerics, whose sort order is unambiguous). Missing or
 * undecodable statistics never prune. NULL rows can never satisfy a comparison, so pruning on
 * value ranges alone is sound even for columns that also hold NULLs.
 *
 * @return false only when the statistics PROVE no row of the group can match.
 */
auto RowGroupCanMatch(const format::RowGroup &group, const std::vector<LogicalType> &column_types,
                      const std::vector<ZonePredicate> &predicates) -> bool;

}  // namespace bumblebee
