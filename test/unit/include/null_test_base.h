//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// null_test_base.h
//
// Identification: test/unit/include/null_test_base.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "bumble_base_test.h"
#include "common/config.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * Shared helpers for the NULL-aware tests.
 *
 * Builds flat vectors with the validity bits cleared at given positions, and asserts
 * nullness through the mask rather than through any sentinel in the data.
 */
class NullTestBase : public BumbleBaseTest {
 protected:
  /**
   * @brief A FLAT Vector of `count` rows of `type`, NULL at the listed positions.
   *
   * The non-null rows hold 1, 2, ... so that row `i` is never confusable with a zero fill.
   *
   * @param type The type of the values.
   * @param count The number of rows.
   * @param null_positions The rows to mark NULL.
   * @return Vector The vector.
   */
  auto CreateVectorWithNulls(const LogicalType &type, idx_t count, const std::vector<idx_t> &null_positions) -> Vector {
    Vector v(type, count);
    for (idx_t i = 0; i < count; i++) {
      v.SetValue(i, Value(static_cast<int64_t>(i + 1)).CastAs(type));
    }
    for (auto pos : null_positions) {
      v.SetValue(pos, Value::Null(type));
    }
    return v;
  }

  /** @return True if the logical row `idx` of `v` is NULL, read through the mask. */
  static auto IsNull(const Vector &v, idx_t idx) -> bool { return !v.RowIsValid(idx); }

  /**
   * @brief A reproducible null layout: roughly `density` of [0, count) marked null.
   *
   * @param count The number of rows.
   * @param density The fraction of rows to mark null, in [0, 1].
   * @param seed The RNG seed, so that a layout can be reproduced.
   * @return std::vector<idx_t> The positions to mark null, ascending.
   */
  auto RandomNullPlacement(idx_t count, double density, std::uint64_t seed) -> std::vector<idx_t> {
    std::mt19937_64 gen(seed);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    std::vector<idx_t> positions;
    for (idx_t i = 0; i < count; i++) {
      if (dist(gen) < density) {
        positions.push_back(i);
      }
    }
    return positions;
  }
};

}  // namespace bumblebee
