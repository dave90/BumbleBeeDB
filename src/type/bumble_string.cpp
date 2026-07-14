//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumble_string.cpp
//
// Identification: src/type/bumble_string.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/bumble_string.h"

namespace bumblebee {

auto BumbleString::GetString() const -> std::string {
  const char *data = GetDataUnsafe();
  return {data, Size()};
}

}  // namespace bumblebee
