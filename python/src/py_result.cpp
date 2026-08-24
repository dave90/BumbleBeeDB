//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// py_result.cpp
//
// Identification: python/src/py_result.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/py_result.h"

#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/python_conversion.h"
#include "type/logical_type.h"

namespace py = pybind11;

namespace bumblebee::python {

PyResult::PyResult(QueryResult result) : result_(std::make_shared<QueryResult>(std::move(result))) {
  // Stage immutable chunks exactly once while the caller's native/GIL-release scope is active.
  // Cursor and DataFrame conversion never revisit engine vectors while holding the GIL.
  staged_columns_.resize(result_->Types().size());
  for (auto &column : staged_columns_) {
    column.reserve(result_->RowCount());
  }
  for (const auto &chunk : result_->Chunks()) {
    for (idx_t row = 0; row < chunk->GetSize(); row++) {
      for (idx_t column = 0; column < chunk->ColumnCount(); column++) {
        staged_columns_[column].push_back(chunk->GetValue(column, row));
      }
    }
  }
}

auto PyResult::Columns() const -> std::vector<std::string> { return result_->Columns(); }

auto PyResult::Types() const -> std::vector<std::string> {
  std::vector<std::string> types;
  types.reserve(result_->Types().size());
  for (const auto &type : result_->Types()) {
    types.push_back(type.ToString());
  }
  return types;
}

auto PyResult::RowCount() const -> idx_t { return result_->RowCount(); }

auto PyResult::IsCommand() const -> bool { return result_->IsCommand(); }

auto PyResult::CommandTag() const -> std::string { return result_->CommandTag(); }

auto PyResult::Status() const -> std::string { return result_->Status(); }

auto PyResult::AffectedRows() const -> std::optional<int64_t> { return result_->AffectedRows(); }

auto PyResult::Tuples() const -> py::list { return BoxRows(0, result_->RowCount()); }

auto PyResult::FetchOne() -> py::object {
  if (cursor_position_ >= result_->RowCount()) {
    return py::none();
  }
  return BoxRow(cursor_position_++);
}

auto PyResult::FetchMany(py::ssize_t size) -> py::list {
  if (size < 0) {
    throw py::value_error("fetchmany size must be non-negative");
  }
  const auto remaining = result_->RowCount() - cursor_position_;
  const auto requested = static_cast<uint64_t>(size);
  const auto count = static_cast<idx_t>(std::min<uint64_t>(remaining, requested));
  const auto end = cursor_position_ + count;
  auto rows = BoxRows(cursor_position_, end);
  cursor_position_ = end;
  return rows;
}

auto PyResult::FetchAll() -> py::list {
  auto rows = BoxRows(cursor_position_, result_->RowCount());
  cursor_position_ = result_->RowCount();
  return rows;
}

auto PyResult::Next() -> py::object {
  if (cursor_position_ >= result_->RowCount()) {
    throw py::stop_iteration();
  }
  return BoxRow(cursor_position_++);
}

auto PyResult::ToDataFrame() const -> py::object {
  auto pandas = py::module_::import("pandas");
  auto numpy = py::module_::import("numpy");
  PythonConversionContext context;
  py::list series;

  for (size_t column = 0; column < staged_columns_.size(); column++) {
    const auto &type = result_->Types()[column];
    py::list values;
    bool has_null = false;
    for (const auto &value : staged_columns_[column]) {
      has_null = has_null || value.IsNull();
      values.append(ValueToPython(value, context));
    }

    py::object array;
    switch (type.GetTypeId()) {
      case LogicalTypeId::BOOLEAN:
        array = has_null ? pandas.attr("array")(values, py::arg("dtype") = "boolean")
                         : numpy.attr("array")(values, py::arg("dtype") = "bool");
        break;
      case LogicalTypeId::TINYINT:
        array = IntegerArray(pandas, numpy, values, has_null, "Int8", "int8");
        break;
      case LogicalTypeId::SMALLINT:
        array = IntegerArray(pandas, numpy, values, has_null, "Int16", "int16");
        break;
      case LogicalTypeId::INTEGER:
        array = IntegerArray(pandas, numpy, values, has_null, "Int32", "int32");
        break;
      case LogicalTypeId::BIGINT:
        array = IntegerArray(pandas, numpy, values, has_null, "Int64", "int64");
        break;
      case LogicalTypeId::UTINYINT:
        array = IntegerArray(pandas, numpy, values, has_null, "UInt8", "uint8");
        break;
      case LogicalTypeId::USMALLINT:
        array = IntegerArray(pandas, numpy, values, has_null, "UInt16", "uint16");
        break;
      case LogicalTypeId::UINTEGER:
        array = IntegerArray(pandas, numpy, values, has_null, "UInt32", "uint32");
        break;
      case LogicalTypeId::UBIGINT:
      case LogicalTypeId::HASH:
      case LogicalTypeId::ADDRESS:
        array = IntegerArray(pandas, numpy, values, has_null, "UInt64", "uint64");
        break;
      case LogicalTypeId::FLOAT:
        array = numpy.attr("array")(values, py::arg("dtype") = "float32");
        break;
      case LogicalTypeId::DOUBLE:
        array = numpy.attr("array")(values, py::arg("dtype") = "float64");
        break;
      case LogicalTypeId::TIMESTAMP:
        array = pandas.attr("array")(values, py::arg("dtype") = "datetime64[us]");
        break;
      default:
        // Strings, exact decimals, dates and nested values intentionally remain Python objects.
        array = pandas.attr("array")(values, py::arg("dtype") = "object");
        break;
    }
    series.append(pandas.attr("Series")(array, py::arg("name") = result_->Columns()[column]));
  }

  if (py::len(series) == 0) {
    return pandas.attr("DataFrame")();
  }
  return pandas.attr("concat")(series, py::arg("axis") = 1);
}

auto PyResult::IntegerArray(const py::object &pandas, const py::object &numpy, const py::list &values, bool has_null,
                            const char *nullable_dtype, const char *numpy_dtype) -> py::object {
  if (has_null) {
    return pandas.attr("array")(values, py::arg("dtype") = nullable_dtype);
  }
  return numpy.attr("array")(values, py::arg("dtype") = numpy_dtype);
}

auto PyResult::BoxRow(idx_t row) const -> py::tuple {
  PythonConversionContext context;
  py::tuple tuple(staged_columns_.size());
  for (size_t column = 0; column < staged_columns_.size(); column++) {
    tuple[column] = ValueToPython(staged_columns_[column][row], context);
  }
  return tuple;
}

auto PyResult::BoxRows(idx_t begin, idx_t end) const -> py::list {
  PythonConversionContext context;
  py::list output;
  for (idx_t row = begin; row < end; row++) {
    py::tuple tuple(staged_columns_.size());
    for (size_t column = 0; column < staged_columns_.size(); column++) {
      tuple[column] = ValueToPython(staged_columns_[column][row], context);
    }
    output.append(std::move(tuple));
  }
  return output;
}

}  // namespace bumblebee::python
