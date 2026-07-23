//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// aggregate_update_kernels.h
//
// Identification: src/include/execution/aggregate/aggregate_update_kernels.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"
#include "execution/plans/aggregation_plan.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * @brief Fold one aggregate's whole argument vector into row-embedded aggregate states.
 *
 * The grouped-aggregation hot loop: each of the `count` rows updates the state of ITS group — a
 * `(count BIGINT, value DOUBLE)` pair living inside the group's hash-table row at
 * `addrs[i] + cnt_off` / `addrs[i] + val_off`. Columnar and boxing-free: COUNT(*) never reads the
 * argument at all; a CONSTANT argument hoists the value and its NULL check out of the loop; any
 * other encoding is normalified once so the typed kernels stream direct contiguous data, with the
 * validity check hoisted into an all-valid vs masked instantiation. Values accumulate as double
 * (SUM) or keep the extreme as double (MIN/MAX); "state initialized" is simply count > 0.
 *
 * @param type    Which aggregate (COUNT(*)/COUNT/SUM/MIN/MAX).
 * @param arg     The argument vector (may be normalified in place).
 * @param addrs   Per input row, the address of its group's row (from FindOrCreateGroups).
 * @param count   How many input rows.
 * @param cnt_off Byte offset of the aggregate's count slot within a group row.
 * @param val_off Byte offset of the aggregate's value slot within a group row.
 */
void UpdateOneAggregate(AggregationType type, Vector &arg, data_ptr_t *addrs, idx_t count, idx_t cnt_off,
                        idx_t val_off);

}  // namespace bumblebee
