//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// python_conversion.cpp
//
// Identification: python/src/python_conversion.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/python_conversion.h"

#include <pybind11/stl.h>

#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/exception.h"
#include "type/date.h"
#include "type/timestamp.h"

namespace py = pybind11;

namespace bumblebee::python {

auto PythonConversionContext::DecimalClass() -> py::object {
  if (!decimal_) {
    decimal_ = py::module_::import("decimal").attr("Decimal");
  }
  return decimal_;
}

auto PythonConversionContext::DateClass() -> py::object {
  if (!date_) {
    date_ = py::module_::import("datetime").attr("date");
  }
  return date_;
}

auto PythonConversionContext::DatetimeClass() -> py::object {
  if (!datetime_) {
    datetime_ = py::module_::import("datetime").attr("datetime");
  }
  return datetime_;
}

static auto DecimalToPython(const Value &value, PythonConversionContext &context) -> py::object {
  int64_t raw = 0;
  switch (value.GetPhysicalType()) {
    case PhysicalType::SMALLINT:
      raw = value.GetAs<int16_t>();
      break;
    case PhysicalType::INTEGER:
      raw = value.GetAs<int32_t>();
      break;
    case PhysicalType::BIGINT:
      raw = value.GetAs<int64_t>();
      break;
    default:
      throw DataException("invalid native DECIMAL backing type");
  }
  const auto scale = value.GetType().GetDecimalData().scale_;
  auto decimal = context.DecimalClass()(py::int_(raw));
  return decimal.attr("scaleb")(py::int_(-scale));
}

static auto DateToPython(const Value &value, PythonConversionContext &context) -> py::object {
  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
  Date::Convert(value.GetAs<int32_t>(), year, month, day);
  if (year < 1 || year > 9999) {
    throw DataException("DATE is outside Python datetime.date's supported year range");
  }
  return context.DateClass()(year, month, day);
}

static auto TimestampToPython(const Value &value, PythonConversionContext &context) -> py::object {
  date_t date = 0;
  int64_t time = 0;
  Timestamp::Convert(value.GetAs<timestamp_t>(), date, time);

  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
  Date::Convert(date, year, month, day);
  if (year < 1 || year > 9999) {
    throw DataException("TIMESTAMP is outside Python datetime.datetime's supported year range");
  }

  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  int32_t micros = 0;
  Timestamp::Convert(time, hour, minute, second, micros);
  return context.DatetimeClass()(year, month, day, hour, minute, second, micros);
}

auto ValueToPython(const Value &value, PythonConversionContext &context) -> py::object {
  if (value.IsNull()) {
    return py::none();
  }
  switch (value.GetType().GetTypeId()) {
    case LogicalTypeId::BOOLEAN:
      return py::bool_(value.GetAs<bool>());
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
      return py::int_(value.GetAs<int64_t>());
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
      return py::int_(value.GetAs<uint64_t>());
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
      return py::float_(value.GetAs<double>());
    case LogicalTypeId::STRING:
      return py::cast(value.GetString());
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY: {
      py::list result;
      for (const auto &child : value.GetChildren()) {
        result.append(ValueToPython(child, context));
      }
      return std::move(result);
    }
    case LogicalTypeId::DECIMAL:
      return DecimalToPython(value, context);
    case LogicalTypeId::DATE:
      return DateToPython(value, context);
    case LogicalTypeId::TIMESTAMP:
      return TimestampToPython(value, context);
    case LogicalTypeId::STRUCT: {
      py::tuple result(value.GetChildren().size());
      for (size_t child = 0; child < value.GetChildren().size(); child++) {
        result[child] = ValueToPython(value.GetChildren()[child], context);
      }
      return std::move(result);
    }
    case LogicalTypeId::UNKNOWN:
      throw DataException("cannot convert a non-NULL value with UNKNOWN logical type");
  }
  throw DataException("cannot convert an unsupported BumbleBeeDB value to Python");
}

auto TableMetadataToPython(const TableMetadata &metadata) -> py::dict {
  py::list columns;
  for (const auto &column : metadata.columns_) {
    py::dict item;
    item["name"] = column.name_;
    item["type"] = column.type_.ToString();
    item["primary_key"] = column.primary_key_;
    columns.append(std::move(item));
  }

  py::dict result;
  result["name"] = metadata.name_;
  result["columns"] = std::move(columns);
  result["primary_key"] = metadata.primary_key_;
  result["generated_id"] = metadata.generated_id_;
  result["storage"] = metadata.storage_format_ == StorageFormat::PARQUET ? "parquet" : "row";
  result["location"] = metadata.location_.has_value() ? py::cast(*metadata.location_) : py::none();
  result["estimated_rows"] = metadata.estimated_rows_;
  return result;
}

auto TableMetadataListToPython(const std::vector<TableMetadata> &metadata) -> py::list {
  py::list result;
  for (const auto &table : metadata) {
    result.append(TableMetadataToPython(table));
  }
  return result;
}

}  // namespace bumblebee::python
