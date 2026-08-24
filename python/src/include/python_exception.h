//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// python_exception.h
//
// Identification: python/src/include/python_exception.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

namespace bumblebee::python {

void RegisterExceptions(pybind11::module_ &module);

}  // namespace bumblebee::python
