//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// macros.h
//
// Identification: src/include/common/macros.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace bumblebee {

#define BUMBLEBEE_ASSERT(expr, message) assert((expr) && (message))

#define BUMBLEBEE_ENSURE(expr, message)               \
  if (!(expr)) {                                      \
    std::cerr << "ERROR: " << (message) << std::endl; \
    std::terminate();                                 \
  }

#define UNIMPLEMENTED(message) throw std::logic_error(message)

#define UNREACHABLE(message) throw std::logic_error(message)

}  // namespace bumblebee
