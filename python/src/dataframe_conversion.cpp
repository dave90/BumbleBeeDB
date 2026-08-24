//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// dataframe_conversion.cpp
//
// Identification: python/src/dataframe_conversion.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/dataframe_conversion.h"

#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/exception.h"
#include "include/python_conversion.h"
#include "type/date.h"
#include "type/timestamp.h"
#include "type/value.h"

namespace py = pybind11;

namespace bumblebee::python {

enum class RawColumnKind : uint8_t {
  NONE,
  BOOL,
  INT8,
  INT16,
  INT32,
  INT64,
  UINT8,
  UINT16,
  UINT32,
  UINT64,
  FLOAT32,
  FLOAT64,
  DATETIME_US,
};

struct StagedDataFrameColumn {
  std::string name_;
  LogicalType type_;
  RawColumnKind raw_kind_{RawColumnKind::NONE};
  // The owner keeps a captured NumPy buffer alive while native chunk construction runs without the
  // GIL. No Python operation or reference-count mutation occurs inside that native scope.
  py::object raw_owner_;
  const char *raw_data_{nullptr};
  py::ssize_t raw_stride_{0};
  std::vector<Value> values_;
};

struct StagedDataFrame {
  idx_t row_count_{0};
  std::vector<StagedDataFrameColumn> columns_;
};

template <typename T>
static auto ReadRaw(const StagedDataFrameColumn &column, idx_t row) -> T {
  T result;
  std::memcpy(&result, column.raw_data_ + static_cast<py::ssize_t>(row) * column.raw_stride_, sizeof(T));
  return result;
}

static auto RawValueAt(const StagedDataFrameColumn &column, idx_t row) -> Value {
  switch (column.raw_kind_) {
    case RawColumnKind::BOOL:
      return Value(ReadRaw<uint8_t>(column, row) != 0);
    case RawColumnKind::INT8:
      return Value(ReadRaw<int8_t>(column, row));
    case RawColumnKind::INT16:
      return Value(ReadRaw<int16_t>(column, row));
    case RawColumnKind::INT32:
      return Value(ReadRaw<int32_t>(column, row));
    case RawColumnKind::INT64:
      return Value(ReadRaw<int64_t>(column, row));
    case RawColumnKind::UINT8:
      return Value(ReadRaw<uint8_t>(column, row));
    case RawColumnKind::UINT16:
      return Value(ReadRaw<uint16_t>(column, row));
    case RawColumnKind::UINT32:
      return Value(ReadRaw<uint32_t>(column, row));
    case RawColumnKind::UINT64:
      return Value(ReadRaw<uint64_t>(column, row));
    case RawColumnKind::FLOAT32: {
      const auto value = ReadRaw<float>(column, row);
      return std::isnan(value) ? Value::Null(column.type_) : Value(value);
    }
    case RawColumnKind::FLOAT64: {
      const auto value = ReadRaw<double>(column, row);
      return std::isnan(value) ? Value::Null(column.type_) : Value(value);
    }
    case RawColumnKind::DATETIME_US: {
      const auto value = ReadRaw<int64_t>(column, row);
      if (value == std::numeric_limits<int64_t>::min()) {  // NumPy's NaT sentinel.
        return Value::Null(column.type_);
      }
      return Value(value).CastAs(column.type_);
    }
    case RawColumnKind::NONE:
      break;
  }
  throw DataException("invalid raw DataFrame staging kind");
}

static auto BuildOwnedChunks(const StagedDataFrame &staged) -> data_chunk_vector_t {
  std::vector<LogicalType> types;
  types.reserve(staged.columns_.size());
  for (const auto &column : staged.columns_) {
    types.push_back(column.type_);
  }

  data_chunk_vector_t chunks;
  for (idx_t offset = 0; offset < staged.row_count_; offset += STANDARD_VECTOR_SIZE) {
    const auto count = std::min<idx_t>(STANDARD_VECTOR_SIZE, staged.row_count_ - offset);
    auto chunk = std::make_unique<DataChunk>();
    chunk->Initialize(types);
    for (idx_t column = 0; column < staged.columns_.size(); column++) {
      const auto &source = staged.columns_[column];
      for (idx_t row = 0; row < count; row++) {
        const auto source_row = offset + row;
        chunk->SetValue(
            column, row,
            source.raw_kind_ == RawColumnKind::NONE ? source.values_[source_row] : RawValueAt(source, source_row));
      }
    }
    chunk->SetCardinality(count);
    chunks.push_back(std::move(chunk));
  }
  return chunks;
}

static auto IsMissingPythonValue(const py::handle &value, const py::object &pandas) -> bool {
  if (value.is_none()) {
    return true;
  }
  auto marker = pandas.attr("isna")(value);
  try {
    return marker.cast<bool>();
  } catch (const py::cast_error &) {
    throw DataException("DataFrame cell has a non-scalar missing-value marker");
  }
}

static auto ReadPythonSigned(const py::handle &value, int64_t minimum, int64_t maximum) -> int64_t {
  if (PyBool_Check(value.ptr()) != 0 || PyLong_Check(value.ptr()) == 0) {
    throw DataException("expected an integer DataFrame value");
  }
  const auto parsed = PyLong_AsLongLong(value.ptr());
  if (PyErr_Occurred() != nullptr) {
    PyErr_Clear();
    throw DataException("signed integer DataFrame value is out of range");
  }
  if (parsed < minimum || parsed > maximum) {
    throw DataException("signed integer DataFrame value is out of range");
  }
  return parsed;
}

static auto ReadPythonUnsigned(const py::handle &value, uint64_t maximum) -> uint64_t {
  if (PyBool_Check(value.ptr()) != 0 || PyLong_Check(value.ptr()) == 0) {
    throw DataException("expected an unsigned integer DataFrame value");
  }
  const auto parsed = PyLong_AsUnsignedLongLong(value.ptr());
  if (PyErr_Occurred() != nullptr) {
    PyErr_Clear();
    throw DataException("unsigned integer DataFrame value is out of range");
  }
  if (parsed > maximum) {
    throw DataException("unsigned integer DataFrame value is out of range");
  }
  return parsed;
}

struct DecimalComponents {
  int64_t magnitude_{0};
  int scale_{0};
  bool negative_{false};
};

static auto PythonDecimalComponents(const py::handle &value, const py::object &decimal_class) -> DecimalComponents {
  if (!py::isinstance(value, decimal_class)) {
    throw DataException("expected decimal.Decimal values in DECIMAL DataFrame column");
  }
  if (!value.attr("is_finite")().cast<bool>()) {
    throw DataException("non-finite decimal.Decimal values are not supported");
  }
  const auto parts = value.attr("as_tuple")().cast<py::tuple>();
  const bool negative = parts[0].cast<int>() != 0;
  const auto digits = parts[1].cast<py::tuple>();
  const int exponent = parts[2].cast<int>();

  int64_t magnitude = 0;
  for (const auto &digit : digits) {
    const int next = py::cast<int>(digit);
    if (magnitude > (std::numeric_limits<int64_t>::max() - next) / 10) {
      throw DataException("decimal.Decimal exceeds BumbleBeeDB's 18-digit precision");
    }
    magnitude = magnitude * 10 + next;
  }
  const int scale = std::max(-exponent, 0);
  for (int zero = 0; zero < std::max(exponent, 0); zero++) {
    if (magnitude > std::numeric_limits<int64_t>::max() / 10) {
      throw DataException("decimal.Decimal exceeds BumbleBeeDB's 18-digit precision");
    }
    magnitude *= 10;
  }
  return {magnitude, scale, negative};
}

static auto PythonDateValue(const py::handle &value, const py::object &date_class) -> Value {
  if (!py::isinstance(value, date_class)) {
    throw DataException("expected datetime.date values in DATE DataFrame column");
  }
  const auto year = value.attr("year").cast<int32_t>();
  const auto month = value.attr("month").cast<int32_t>();
  const auto day = value.attr("day").cast<int32_t>();
  date_t native_date = 0;
  if (!Date::TryFromDate(year, month, day, native_date)) {
    throw DataException("datetime.date is outside BumbleBeeDB's DATE range");
  }
  return Value(static_cast<int32_t>(native_date)).CastAs(LogicalType::Date());
}

static auto PythonTimestampValue(const py::handle &value, const py::object &date_class,
                                 const py::object &datetime_class) -> Value {
  if (!py::isinstance(value, date_class)) {
    throw DataException("expected datetime.datetime values in TIMESTAMP DataFrame column");
  }

  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  int32_t microsecond = 0;
  if (py::isinstance(value, datetime_class)) {
    const auto tzinfo = value.attr("tzinfo");
    if (!tzinfo.is_none() && !value.attr("utcoffset")().is_none()) {
      throw DataException("timezone-aware datetimes are not supported; convert to a naive timestamp first");
    }
    hour = value.attr("hour").cast<int32_t>();
    minute = value.attr("minute").cast<int32_t>();
    second = value.attr("second").cast<int32_t>();
    microsecond = value.attr("microsecond").cast<int32_t>();
  }

  date_t native_date = 0;
  if (!Date::TryFromDate(value.attr("year").cast<int32_t>(), value.attr("month").cast<int32_t>(),
                         value.attr("day").cast<int32_t>(), native_date)) {
    throw DataException("datetime value is outside BumbleBeeDB's TIMESTAMP range");
  }
  const int64_t time = static_cast<int64_t>(hour) * Timestamp::MICROS_PER_HOUR +
                       static_cast<int64_t>(minute) * Timestamp::MICROS_PER_MINUTE +
                       static_cast<int64_t>(second) * Timestamp::MICROS_PER_SEC + microsecond;
  const int64_t timestamp = static_cast<int64_t>(native_date) * Timestamp::MICROS_PER_DAY + time;
  return Value(timestamp).CastAs(LogicalType::Timestamp());
}

static auto PythonValueForType(const py::handle &value, const LogicalType &type, PythonConversionContext &context,
                               int decimal_scale = 0) -> Value {
  switch (type.GetTypeId()) {
    case LogicalTypeId::BOOLEAN:
      if (PyBool_Check(value.ptr()) == 0) {
        throw DataException("expected bool values in BOOLEAN DataFrame column");
      }
      return Value(value.cast<bool>());
    case LogicalTypeId::TINYINT:
      return Value(static_cast<int8_t>(ReadPythonSigned(value, INT8_MIN, INT8_MAX)));
    case LogicalTypeId::SMALLINT:
      return Value(static_cast<int16_t>(ReadPythonSigned(value, INT16_MIN, INT16_MAX)));
    case LogicalTypeId::INTEGER:
      return Value(static_cast<int32_t>(ReadPythonSigned(value, INT32_MIN, INT32_MAX)));
    case LogicalTypeId::BIGINT:
      return Value(ReadPythonSigned(value, INT64_MIN, INT64_MAX));
    case LogicalTypeId::UTINYINT:
      return Value(static_cast<uint8_t>(ReadPythonUnsigned(value, UINT8_MAX)));
    case LogicalTypeId::USMALLINT:
      return Value(static_cast<uint16_t>(ReadPythonUnsigned(value, UINT16_MAX)));
    case LogicalTypeId::UINTEGER:
      return Value(static_cast<uint32_t>(ReadPythonUnsigned(value, UINT32_MAX)));
    case LogicalTypeId::UBIGINT:
      return Value(ReadPythonUnsigned(value, UINT64_MAX));
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE: {
      if (PyFloat_Check(value.ptr()) == 0 && PyLong_Check(value.ptr()) == 0) {
        throw DataException("expected numeric values in floating DataFrame column");
      }
      const auto number = PyFloat_AsDouble(value.ptr());
      if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        throw DataException("floating DataFrame value is out of range");
      }
      return type.GetTypeId() == LogicalTypeId::FLOAT ? Value(static_cast<float>(number)) : Value(number);
    }
    case LogicalTypeId::DECIMAL: {
      const auto parts = PythonDecimalComponents(value, context.DecimalClass());
      int64_t magnitude = parts.magnitude_;
      for (int zero = parts.scale_; zero < decimal_scale; zero++) {
        if (magnitude > std::numeric_limits<int64_t>::max() / 10) {
          throw DataException("decimal.Decimal exceeds BumbleBeeDB's 18-digit precision");
        }
        magnitude *= 10;
      }
      const auto raw = parts.negative_ ? -magnitude : magnitude;
      return Value(raw).CastAs(type);
    }
    case LogicalTypeId::STRING:
      if (PyUnicode_Check(value.ptr()) == 0) {
        throw DataException("expected str values in VARCHAR DataFrame column");
      }
      return Value(value.cast<std::string>());
    case LogicalTypeId::DATE:
      return PythonDateValue(value, context.DateClass());
    case LogicalTypeId::TIMESTAMP:
      return PythonTimestampValue(value, context.DateClass(), context.DatetimeClass());
    default:
      throw DataException(fmt::format("DataFrame ingestion does not support {} columns", type.ToString()));
  }
}

enum class PythonColumnKind : uint8_t { UNKNOWN, BOOLEAN, INTEGER, FLOAT, DECIMAL, STRING, DATE, TIMESTAMP };

static auto ClassifyPythonValue(const py::handle &value, PythonConversionContext &context) -> PythonColumnKind {
  if (py::isinstance(value, context.DatetimeClass())) {
    return PythonColumnKind::TIMESTAMP;
  }
  if (py::isinstance(value, context.DateClass())) {
    return PythonColumnKind::DATE;
  }
  if (py::isinstance(value, context.DecimalClass())) {
    return PythonColumnKind::DECIMAL;
  }
  if (PyBool_Check(value.ptr()) != 0) {
    return PythonColumnKind::BOOLEAN;
  }
  if (PyLong_Check(value.ptr()) != 0) {
    return PythonColumnKind::INTEGER;
  }
  if (PyFloat_Check(value.ptr()) != 0) {
    return PythonColumnKind::FLOAT;
  }
  if (PyUnicode_Check(value.ptr()) != 0) {
    return PythonColumnKind::STRING;
  }
  throw DataException(fmt::format("unsupported Python value type '{}' in DataFrame column",
                                  py::str(py::type::of(value)).cast<std::string>()));
}

static auto MergePythonKinds(PythonColumnKind left, PythonColumnKind right) -> PythonColumnKind {
  if (left == PythonColumnKind::UNKNOWN || left == right) {
    return right;
  }
  if ((left == PythonColumnKind::INTEGER && right == PythonColumnKind::FLOAT) ||
      (left == PythonColumnKind::FLOAT && right == PythonColumnKind::INTEGER)) {
    return PythonColumnKind::FLOAT;
  }
  if ((left == PythonColumnKind::DATE && right == PythonColumnKind::TIMESTAMP) ||
      (left == PythonColumnKind::TIMESTAMP && right == PythonColumnKind::DATE)) {
    return PythonColumnKind::TIMESTAMP;
  }
  throw DataException("mixed incompatible Python value types in one DataFrame column");
}

static auto TypeForDtypeName(const std::string &name) -> std::optional<LogicalType> {
  if (name == "bool" || name == "boolean") return LogicalType(LogicalTypeId::BOOLEAN);
  if (name == "int8" || name == "Int8") return LogicalType(LogicalTypeId::TINYINT);
  if (name == "int16" || name == "Int16") return LogicalType(LogicalTypeId::SMALLINT);
  if (name == "int32" || name == "Int32") return LogicalType(LogicalTypeId::INTEGER);
  if (name == "int64" || name == "Int64") return LogicalType(LogicalTypeId::BIGINT);
  if (name == "uint8" || name == "UInt8") return LogicalType(LogicalTypeId::UTINYINT);
  if (name == "uint16" || name == "UInt16") return LogicalType(LogicalTypeId::USMALLINT);
  if (name == "uint32" || name == "UInt32") return LogicalType(LogicalTypeId::UINTEGER);
  if (name == "uint64" || name == "UInt64") return LogicalType(LogicalTypeId::UBIGINT);
  if (name == "float32" || name == "Float32") return LogicalType(LogicalTypeId::FLOAT);
  if (name == "float64" || name == "Float64") return LogicalType(LogicalTypeId::DOUBLE);
  if (name == "str" || name == "string" || name.rfind("string[", 0) == 0) {
    return LogicalType(LogicalTypeId::STRING);
  }
  if (name.rfind("datetime64[", 0) == 0 && name.find(',') == std::string::npos) {
    return LogicalType::Timestamp();
  }
  return std::nullopt;
}

static auto RawKindForDtypeName(const std::string &name) -> RawColumnKind {
  if (name == "bool") return RawColumnKind::BOOL;
  if (name == "int8") return RawColumnKind::INT8;
  if (name == "int16") return RawColumnKind::INT16;
  if (name == "int32") return RawColumnKind::INT32;
  if (name == "int64") return RawColumnKind::INT64;
  if (name == "uint8") return RawColumnKind::UINT8;
  if (name == "uint16") return RawColumnKind::UINT16;
  if (name == "uint32") return RawColumnKind::UINT32;
  if (name == "uint64") return RawColumnKind::UINT64;
  if (name == "float32") return RawColumnKind::FLOAT32;
  if (name == "float64") return RawColumnKind::FLOAT64;
  if (name.rfind("datetime64[", 0) == 0 && name.find(',') == std::string::npos) {
    return RawColumnKind::DATETIME_US;
  }
  return RawColumnKind::NONE;
}

static auto RawKindSize(RawColumnKind kind) -> py::ssize_t {
  switch (kind) {
    case RawColumnKind::BOOL:
    case RawColumnKind::INT8:
    case RawColumnKind::UINT8:
      return 1;
    case RawColumnKind::INT16:
    case RawColumnKind::UINT16:
      return 2;
    case RawColumnKind::INT32:
    case RawColumnKind::UINT32:
    case RawColumnKind::FLOAT32:
      return 4;
    case RawColumnKind::INT64:
    case RawColumnKind::UINT64:
    case RawColumnKind::FLOAT64:
    case RawColumnKind::DATETIME_US:
      return 8;
    case RawColumnKind::NONE:
      return 0;
  }
  return 0;
}

static void CaptureRawColumn(const py::object &series, RawColumnKind kind, idx_t row_count,
                             StagedDataFrameColumn &column) {
  py::object array;
  if (kind == RawColumnKind::DATETIME_US) {
    array = series.attr("to_numpy")(py::arg("dtype") = "datetime64[us]", py::arg("copy") = false).attr("view")("int64");
  } else {
    array = series.attr("to_numpy")(py::arg("copy") = false);
  }
  auto buffer = py::cast<py::buffer>(array);
  const auto info = buffer.request();
  if (info.ndim != 1 || static_cast<idx_t>(info.shape[0]) != row_count || info.itemsize != RawKindSize(kind)) {
    throw DataException("numeric DataFrame column exposed an unexpected buffer layout");
  }
  column.raw_kind_ = kind;
  column.raw_owner_ = std::move(array);
  column.raw_data_ = static_cast<const char *>(info.ptr);
  column.raw_stride_ = info.strides[0];
}

static auto InferObjectType(const py::list &values, const py::object &pandas, PythonConversionContext &context)
    -> LogicalType {
  PythonColumnKind kind = PythonColumnKind::UNKNOWN;
  bool saw_negative = false;
  bool needs_unsigned = false;
  int decimal_scale = 0;

  for (const auto &value : values) {
    if (IsMissingPythonValue(value, pandas)) {
      continue;
    }
    const auto value_kind = ClassifyPythonValue(value, context);
    kind = MergePythonKinds(kind, value_kind);
    if (value_kind == PythonColumnKind::INTEGER) {
      const auto parsed = PyLong_AsLongLong(value.ptr());
      if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        const auto unsigned_value = PyLong_AsUnsignedLongLong(value.ptr());
        if (PyErr_Occurred() != nullptr) {
          PyErr_Clear();
          throw DataException("Python integer exceeds BumbleBeeDB's 64-bit range");
        }
        static_cast<void>(unsigned_value);
        needs_unsigned = true;
      } else {
        saw_negative = saw_negative || parsed < 0;
      }
    } else if (value_kind == PythonColumnKind::DECIMAL) {
      decimal_scale = std::max(decimal_scale, PythonDecimalComponents(value, context.DecimalClass()).scale_);
    }
  }

  switch (kind) {
    case PythonColumnKind::UNKNOWN:
      // An all-missing object column has no recoverable dtype; VARCHAR is the deterministic schema.
      return LogicalType(LogicalTypeId::STRING);
    case PythonColumnKind::BOOLEAN:
      return LogicalType(LogicalTypeId::BOOLEAN);
    case PythonColumnKind::INTEGER:
      if (needs_unsigned && saw_negative) {
        throw DataException("one object integer column mixes negative and values above INT64_MAX");
      }
      return LogicalType(needs_unsigned ? LogicalTypeId::UBIGINT : LogicalTypeId::BIGINT);
    case PythonColumnKind::FLOAT:
      return LogicalType(LogicalTypeId::DOUBLE);
    case PythonColumnKind::STRING:
      return LogicalType(LogicalTypeId::STRING);
    case PythonColumnKind::DATE:
      return LogicalType::Date();
    case PythonColumnKind::TIMESTAMP:
      return LogicalType::Timestamp();
    case PythonColumnKind::DECIMAL: {
      int width = std::max(decimal_scale, 1);
      for (const auto &value : values) {
        if (IsMissingPythonValue(value, pandas)) continue;
        auto parts = PythonDecimalComponents(value, context.DecimalClass());
        int64_t magnitude = parts.magnitude_;
        for (int scale = parts.scale_; scale < decimal_scale; scale++) {
          if (magnitude > std::numeric_limits<int64_t>::max() / 10) {
            throw DataException("decimal.Decimal exceeds BumbleBeeDB's 18-digit precision");
          }
          magnitude *= 10;
        }
        int digits = 1;
        for (auto copy = magnitude; copy >= 10; copy /= 10) digits++;
        width = std::max(width, digits);
      }
      if (width > LogicalType::MAX_DECIMAL_WIDTH_INT64) {
        throw DataException("decimal.Decimal exceeds BumbleBeeDB's 18-digit precision");
      }
      return LogicalType::Decimal(width, decimal_scale);
    }
  }
  throw DataException("could not infer DataFrame object dtype");
}

static auto StageDataFrameColumn(const py::object &series, std::string name, idx_t row_count, const py::object &pandas,
                                 PythonConversionContext &context) -> StagedDataFrameColumn {
  StagedDataFrameColumn column;
  column.name_ = std::move(name);
  const auto dtype_name = py::str(series.attr("dtype")).cast<std::string>();
  if (dtype_name.rfind("datetime64[", 0) == 0 && dtype_name.find(',') != std::string::npos) {
    throw DataException("timezone-aware datetime columns are not supported; remove the timezone first");
  }

  const bool inferred = dtype_name == "object" || dtype_name == "category";
  auto known_type = TypeForDtypeName(dtype_name);
  if (!known_type.has_value() && !inferred) {
    throw DataException(fmt::format("unsupported pandas dtype '{}'", dtype_name));
  }

  const auto raw_kind = RawKindForDtypeName(dtype_name);
  if (raw_kind != RawColumnKind::NONE) {
    column.type_ = *known_type;
    CaptureRawColumn(series, raw_kind, row_count, column);
    return column;
  }

  const auto python_values = series.attr("tolist")().cast<py::list>();
  if (static_cast<idx_t>(py::len(python_values)) != row_count) {
    throw DataException("DataFrame column length changed during load_df");
  }
  column.type_ = inferred ? InferObjectType(python_values, pandas, context) : *known_type;
  column.values_.reserve(row_count);
  const int decimal_scale =
      column.type_.GetTypeId() == LogicalTypeId::DECIMAL ? column.type_.GetDecimalData().scale_ : 0;
  for (const auto &value : python_values) {
    if (IsMissingPythonValue(value, pandas)) {
      column.values_.push_back(Value::Null(column.type_));
    } else {
      column.values_.push_back(PythonValueForType(value, column.type_, context, decimal_scale));
    }
  }
  return column;
}

static auto StageDataFrame(const py::object &frame, bool include_index) -> StagedDataFrame {
  auto pandas = py::module_::import("pandas");
  if (!py::isinstance(frame, pandas.attr("DataFrame"))) {
    throw py::type_error("frame must be a pandas.DataFrame");
  }

  py::object working = frame;
  if (include_index) {
    if (working.attr("index").attr("nlevels").cast<int>() != 1) {
      throw DataException("include_index supports a one-level pandas index only");
    }
    try {
      working = working.attr("reset_index")();
    } catch (py::error_already_set &error) {
      const auto message = std::string(error.what());
      error.restore();
      PyErr_Clear();
      throw DataException("could not include the DataFrame index: " + message);
    }
  }

  StagedDataFrame staged;
  staged.row_count_ = static_cast<idx_t>(py::len(working));
  auto labels = working.attr("columns").attr("tolist")().cast<py::list>();
  if (py::len(labels) == 0) {
    throw DataException("a DataFrame with zero columns cannot be loaded");
  }

  std::unordered_set<std::string> seen;
  std::vector<std::string> names;
  names.reserve(py::len(labels));
  for (const auto &label : labels) {
    if (PyUnicode_Check(label.ptr()) == 0) {
      throw DataException("DataFrame column names must be strings");
    }
    auto name = py::cast<std::string>(label);
    if (!seen.insert(name).second) {
      throw DataException(fmt::format("duplicate DataFrame column name '{}'", name));
    }
    names.push_back(std::move(name));
  }

  PythonConversionContext context;
  staged.columns_.reserve(names.size());
  auto iloc = working.attr("iloc");
  for (size_t column = 0; column < names.size(); column++) {
    auto key = py::make_tuple(py::slice(py::none(), py::none(), py::none()), py::int_(column));
    auto series = iloc.attr("__getitem__")(key);
    staged.columns_.push_back(
        StageDataFrameColumn(series, std::move(names[column]), staged.row_count_, pandas, context));
  }
  return staged;
}

auto ParsePrimaryKey(const py::object &primary_key) -> std::vector<std::string> {
  if (primary_key.is_none()) {
    return {};
  }
  std::vector<std::string> result;
  if (PyUnicode_Check(primary_key.ptr()) != 0) {
    result.push_back(primary_key.cast<std::string>());
  } else if (py::isinstance<py::list>(primary_key) || py::isinstance<py::tuple>(primary_key)) {
    for (const auto &value : primary_key) {
      if (PyUnicode_Check(value.ptr()) == 0) {
        throw py::type_error("primary_key entries must be strings");
      }
      result.push_back(py::cast<std::string>(value));
    }
  } else {
    throw py::type_error("primary_key must be a string, a list/tuple of strings, or None");
  }
  if (result.empty()) {
    throw py::value_error("primary_key must not be empty");
  }
  return result;
}

auto ConvertDataFrame(const py::object &frame, bool include_index) -> ConvertedDataFrame {
  auto staged = StageDataFrame(frame, include_index);

  ConvertedDataFrame converted;
  converted.names_.reserve(staged.columns_.size());
  converted.types_.reserve(staged.columns_.size());
  for (const auto &column : staged.columns_) {
    converted.names_.push_back(column.name_);
    converted.types_.push_back(column.type_);
  }

  {
    py::gil_scoped_release release;
    converted.chunks_ = BuildOwnedChunks(staged);
  }
  return converted;
}

}  // namespace bumblebee::python
