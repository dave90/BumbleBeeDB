//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// limits.cpp
//
// Identification: src/common/limits.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/limits.h"

#include <limits>

namespace bumblebee {

using std::numeric_limits;

auto NumericLimits<int8_t>::Minimum() -> int8_t { return numeric_limits<int8_t>::lowest(); }
auto NumericLimits<int8_t>::Maximum() -> int8_t { return numeric_limits<int8_t>::max(); }

auto NumericLimits<int16_t>::Minimum() -> int16_t { return numeric_limits<int16_t>::lowest(); }
auto NumericLimits<int16_t>::Maximum() -> int16_t { return numeric_limits<int16_t>::max(); }

auto NumericLimits<int32_t>::Minimum() -> int32_t { return numeric_limits<int32_t>::lowest(); }
auto NumericLimits<int32_t>::Maximum() -> int32_t { return numeric_limits<int32_t>::max(); }

auto NumericLimits<int64_t>::Minimum() -> int64_t { return numeric_limits<int64_t>::lowest(); }
auto NumericLimits<int64_t>::Maximum() -> int64_t { return numeric_limits<int64_t>::max(); }

auto NumericLimits<uint8_t>::Minimum() -> uint8_t { return numeric_limits<uint8_t>::lowest(); }
auto NumericLimits<uint8_t>::Maximum() -> uint8_t { return numeric_limits<uint8_t>::max(); }

auto NumericLimits<uint16_t>::Minimum() -> uint16_t { return numeric_limits<uint16_t>::lowest(); }
auto NumericLimits<uint16_t>::Maximum() -> uint16_t { return numeric_limits<uint16_t>::max(); }

auto NumericLimits<uint32_t>::Minimum() -> uint32_t { return numeric_limits<uint32_t>::lowest(); }
auto NumericLimits<uint32_t>::Maximum() -> uint32_t { return numeric_limits<uint32_t>::max(); }

auto NumericLimits<uint64_t>::Minimum() -> uint64_t { return numeric_limits<uint64_t>::lowest(); }
auto NumericLimits<uint64_t>::Maximum() -> uint64_t { return numeric_limits<uint64_t>::max(); }

auto NumericLimits<float>::Minimum() -> float { return numeric_limits<float>::lowest(); }
auto NumericLimits<float>::Maximum() -> float { return numeric_limits<float>::max(); }

auto NumericLimits<double>::Minimum() -> double { return numeric_limits<double>::lowest(); }
auto NumericLimits<double>::Maximum() -> double { return numeric_limits<double>::max(); }

}  // namespace bumblebee
