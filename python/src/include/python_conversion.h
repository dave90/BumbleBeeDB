//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// python_conversion.h
//
// Identification: python/src/include/python_conversion.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

#include <vector>

#include "main/connection.h"
#include "type/value.h"

namespace bumblebee::python {

struct PythonConversionContext {
  auto DecimalClass() -> pybind11::object;
  auto DateClass() -> pybind11::object;
  auto DatetimeClass() -> pybind11::object;

  pybind11::object decimal_;
  pybind11::object date_;
  pybind11::object datetime_;
};

auto ValueToPython(const Value &value, PythonConversionContext &context) -> pybind11::object;
auto TableMetadataToPython(const TableMetadata &metadata) -> pybind11::dict;
auto TableMetadataListToPython(const std::vector<TableMetadata> &metadata) -> pybind11::list;

}  // namespace bumblebee::python
