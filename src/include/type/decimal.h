//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// decimal.h
//
// Identification: src/include/type/decimal.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace bumblebee {

/**
 * The DECIMAL helpers.
 *
 * A DECIMAL is stored as an integer scaled by 10^scale — the width decides which integer
 * width backs it — so rendering one is a matter of putting the point back in.
 */
class Decimal {
 public:
  /** The widest decimal that fits in each backing integer. */
  static constexpr uint8_t MAX_WIDTH_INT16 = 4;
  static constexpr uint8_t MAX_WIDTH_INT32 = 9;
  static constexpr uint8_t MAX_WIDTH_INT64 = 18;
  static constexpr uint8_t MAX_WIDTH_INT128 = 38;
  static constexpr uint8_t MAX_WIDTH_DECIMAL = MAX_WIDTH_INT64;

  /** @brief Render the decimal backed by `value` with `scale` digits after the point. */
  static auto ToString(int16_t value, uint8_t scale) -> std::string;
  static auto ToString(int32_t value, uint8_t scale) -> std::string;
  static auto ToString(int64_t value, uint8_t scale) -> std::string;
};

}  // namespace bumblebee
