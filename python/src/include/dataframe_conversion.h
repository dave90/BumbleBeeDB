//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// dataframe_conversion.h
//
// Identification: python/src/include/dataframe_conversion.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

#include <string>
#include <vector>

#include "type/logical_type.h"
#include "type/vector/data_chunk.h"

namespace bumblebee::python {

struct ConvertedDataFrame {
  std::vector<std::string> names_;
  std::vector<LogicalType> types_;
  data_chunk_vector_t chunks_;
};

auto ConvertDataFrame(const pybind11::object &frame, bool include_index) -> ConvertedDataFrame;
auto ParsePrimaryKey(const pybind11::object &primary_key) -> std::vector<std::string>;

}  // namespace bumblebee::python
