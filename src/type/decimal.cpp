//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// decimal.cpp
//
// Identification: src/type/decimal.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/decimal.h"

#include <memory>

#include "common/numeric_utils.h"

namespace bumblebee {

/**
 * @brief Render the decimal backed by `value` at the given scale.
 *
 * @tparam SIGNED The backing integer type.
 * @tparam UNSIGNED Its unsigned counterpart, which the formatter works in.
 * @param value The backing integer.
 * @param scale The number of digits after the point.
 * @return std::string The rendering.
 */
template <class SIGNED, class UNSIGNED>
static auto TemplatedDecimalToString(SIGNED value, uint8_t scale) -> std::string {
  auto len = DecimalToString::DecimalLength<SIGNED, UNSIGNED>(value, scale);
  auto data = std::make_unique_for_overwrite<char[]>(len + 1);
  DecimalToString::FormatDecimal<SIGNED, UNSIGNED>(value, scale, data.get(), len);
  return {data.get(), static_cast<std::size_t>(len)};
}

auto Decimal::ToString(int16_t value, uint8_t scale) -> std::string {
  return TemplatedDecimalToString<int16_t, uint16_t>(value, scale);
}

auto Decimal::ToString(int32_t value, uint8_t scale) -> std::string {
  return TemplatedDecimalToString<int32_t, uint32_t>(value, scale);
}

auto Decimal::ToString(int64_t value, uint8_t scale) -> std::string {
  return TemplatedDecimalToString<int64_t, uint64_t>(value, scale);
}

}  // namespace bumblebee
