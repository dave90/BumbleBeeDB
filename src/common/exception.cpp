//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// exception.cpp
//
// Identification: src/common/exception.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/exception.h"

namespace bumblebee {

std::atomic<bool> global_disable_exception_print{false};

}  // namespace bumblebee
