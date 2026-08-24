//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// py_result.h
//
// Identification: python/src/include/py_result.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "main/query_result.h"
#include "type/value.h"

namespace bumblebee::python {

class PyResult {
 public:
  explicit PyResult(QueryResult result);

  auto Columns() const -> std::vector<std::string>;
  auto Types() const -> std::vector<std::string>;
  auto RowCount() const -> idx_t;
  auto IsCommand() const -> bool;
  auto CommandTag() const -> std::string;
  auto Status() const -> std::string;
  auto AffectedRows() const -> std::optional<int64_t>;

  auto Tuples() const -> pybind11::list;
  auto FetchOne() -> pybind11::object;
  auto FetchMany(pybind11::ssize_t size) -> pybind11::list;
  auto FetchAll() -> pybind11::list;
  auto Next() -> pybind11::object;
  auto ToDataFrame() const -> pybind11::object;

 private:
  static auto IntegerArray(const pybind11::object &pandas, const pybind11::object &numpy, const pybind11::list &values,
                           bool has_null, const char *nullable_dtype, const char *numpy_dtype) -> pybind11::object;
  auto BoxRow(idx_t row) const -> pybind11::tuple;
  auto BoxRows(idx_t begin, idx_t end) const -> pybind11::list;

  std::shared_ptr<QueryResult> result_;
  std::vector<std::vector<Value>> staged_columns_;
  idx_t cursor_position_{0};
};

}  // namespace bumblebee::python
