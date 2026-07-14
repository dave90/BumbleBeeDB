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

/** Delete the copy constructor and copy assignment operator of `cname`. */
#define DISALLOW_COPY(cname)                                    \
  cname(const cname &) = delete;                   /* NOLINT */ \
  auto operator=(const cname &)->cname & = delete; /* NOLINT */

/** Delete the move constructor and move assignment operator of `cname`. */
#define DISALLOW_MOVE(cname)                               \
  cname(cname &&) = delete;                   /* NOLINT */ \
  auto operator=(cname &&)->cname & = delete; /* NOLINT */

/** Delete both the copy and move operations of `cname`. */
#define DISALLOW_COPY_AND_MOVE(cname) \
  DISALLOW_COPY(cname);               \
  DISALLOW_MOVE(cname);

}  // namespace bumblebee
