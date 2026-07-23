//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// value.cpp
//
// Identification: src/type/value.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/value.h"

#include "type/vector/operations/vector_operations.h"
#include "type/vector/vector.h"

#include <sstream>

#include "common/macros.h"
#include "fmt/ranges.h"

namespace bumblebee {

Value::Value(bool val) : type_(LogicalTypeId::BOOLEAN), is_null_(false) {
  value_.utinyint_ = static_cast<uint8_t>(val ? 1 : 0);
}
Value::Value(int8_t val) : type_(LogicalTypeId::TINYINT), is_null_(false) { value_.tinyint_ = val; }
Value::Value(int16_t val) : type_(LogicalTypeId::SMALLINT), is_null_(false) { value_.smallint_ = val; }
Value::Value(int32_t val) : type_(LogicalTypeId::INTEGER), is_null_(false) { value_.integer_ = val; }
Value::Value(int64_t val) : type_(LogicalTypeId::BIGINT), is_null_(false) { value_.bigint_ = val; }
Value::Value(uint8_t val) : type_(LogicalTypeId::UTINYINT), is_null_(false) { value_.utinyint_ = val; }
Value::Value(uint16_t val) : type_(LogicalTypeId::USMALLINT), is_null_(false) { value_.usmallint_ = val; }
Value::Value(uint32_t val) : type_(LogicalTypeId::UINTEGER), is_null_(false) { value_.uinteger_ = val; }
Value::Value(uint64_t val) : type_(LogicalTypeId::UBIGINT), is_null_(false) { value_.ubigint_ = val; }
Value::Value(float val) : type_(LogicalTypeId::FLOAT), is_null_(false) { value_.float_ = val; }
Value::Value(double val) : type_(LogicalTypeId::DOUBLE), is_null_(false) { value_.double_ = val; }

Value::Value(std::string val)
    : string_value_(std::move(val)), type_(LogicalTypeId::STRING), is_null_(false) {}

Value::Value(const char *val) : Value(std::string(val)) {}

auto Value::Null(const LogicalType &type) -> Value {
  Value value;
  value.type_ = type;
  value.is_null_ = true;
  return value;
}

auto Value::List(const LogicalType &type, std::vector<Value> children) -> Value {
  BUMBLEBEE_ASSERT(type.GetTypeId() == LogicalTypeId::LIST || type.GetTypeId() == LogicalTypeId::ARRAY,
                   "Value::List requires a LIST or ARRAY type");
  Value value;
  value.type_ = type;
  value.children_ = std::move(children);
  value.is_null_ = false;
  return value;
}

auto Value::GetString() const -> const std::string & {
  if (type_.GetPhysicalType() != PhysicalType::STRING) {
    throw Exception(ExceptionType::MISMATCH_TYPE,
                    fmt::format("cannot read {} as a string", type_.ToString()));
  }
  return string_value_;
}

auto Value::ToString() const -> std::string {
  if (is_null_) {
    return "NULL";
  }
  switch (type_.GetTypeId()) {
    case LogicalTypeId::BOOLEAN:
      return value_.utinyint_ != 0 ? "true" : "false";
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::DECIMAL: {
      // Calendar and decimal rendering lives in the vector cast kernels; a 1-row round trip
      // through them keeps this boundary function consistent with chunk-level output.
      Vector src(type_, 1);
      src.SetValue(0, *this);
      Vector dst(LogicalType(LogicalTypeId::STRING), 1);
      VectorOperations::Cast(src, dst, 1);
      return dst.GetValue(0).GetString();
    }
    case LogicalTypeId::STRING:
      return fmt::format("'{}'", string_value_);
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY: {
      std::vector<std::string> parts;
      parts.reserve(children_.size());
      for (const auto &child : children_) {
        parts.emplace_back(child.ToString());
      }
      return fmt::format("[{}]", fmt::join(parts, ", "));
    }
    default:
      break;
  }
  switch (type_.GetPhysicalType()) {
    case PhysicalType::TINYINT:
      return std::to_string(value_.tinyint_);
    case PhysicalType::SMALLINT:
      return std::to_string(value_.smallint_);
    case PhysicalType::INTEGER:
      return std::to_string(value_.integer_);
    case PhysicalType::BIGINT:
      return std::to_string(value_.bigint_);
    case PhysicalType::UTINYINT:
      return std::to_string(value_.utinyint_);
    case PhysicalType::USMALLINT:
      return std::to_string(value_.usmallint_);
    case PhysicalType::UINTEGER:
      return std::to_string(value_.uinteger_);
    case PhysicalType::UBIGINT:
      return std::to_string(value_.ubigint_);
    case PhysicalType::FLOAT:
      return fmt::format("{}", value_.float_);
    case PhysicalType::DOUBLE:
      return fmt::format("{}", value_.double_);
    default:
      return "<unknown>";
  }
}

auto Value::CastAs(const LogicalType &type) const -> Value {
  if (type_ == type) {
    return *this;
  }
  if (is_null_) {
    return Null(type);
  }

  switch (type.GetTypeId()) {
    case LogicalTypeId::BOOLEAN: {
      if (type_.GetPhysicalType() == PhysicalType::STRING) {
        const auto lowered = string_value_;
        return Value(lowered == "true" || lowered == "t" || lowered == "1");
      }
      return Value(GetAs<int64_t>() != 0);
    }
    case LogicalTypeId::TINYINT:
      return Value{GetAs<int8_t>()};
    case LogicalTypeId::SMALLINT:
      return Value{GetAs<int16_t>()};
    case LogicalTypeId::INTEGER:
      return Value{GetAs<int32_t>()};
    case LogicalTypeId::BIGINT:
      return Value{GetAs<int64_t>()};
    case LogicalTypeId::DATE: {
      // Same physical value, stamped with the calendar type so rendering and re-wrapping keep it.
      auto v = Value{GetAs<int32_t>()};
      v.type_ = type;
      return v;
    }
    case LogicalTypeId::TIMESTAMP: {
      auto v = Value{GetAs<int64_t>()};
      v.type_ = type;
      return v;
    }
    case LogicalTypeId::DECIMAL: {
      // The physical payload is already scaled; only the logical identity changes here.
      Value v;
      switch (type.GetPhysicalType()) {
        case PhysicalType::SMALLINT:
          v = Value{GetAs<int16_t>()};
          break;
        case PhysicalType::INTEGER:
          v = Value{GetAs<int32_t>()};
          break;
        default:
          v = Value{GetAs<int64_t>()};
          break;
      }
      v.type_ = type;
      return v;
    }
    case LogicalTypeId::UTINYINT:
      return Value{GetAs<uint8_t>()};
    case LogicalTypeId::USMALLINT:
      return Value{GetAs<uint16_t>()};
    case LogicalTypeId::UINTEGER:
      return Value{GetAs<uint32_t>()};
    case LogicalTypeId::UBIGINT:
      return Value{GetAs<uint64_t>()};
    case LogicalTypeId::FLOAT:
      return Value{GetAs<float>()};
    case LogicalTypeId::DOUBLE:
      return Value{GetAs<double>()};
    case LogicalTypeId::STRING: {
      if (type_.GetPhysicalType() == PhysicalType::STRING) {
        return Value{string_value_};
      }
      // ToString() quotes strings; for a cast we want the bare rendering.
      return Value{ToString()};
    }
    default:
      throw NotImplementedException(
          fmt::format("cannot cast {} to {}", type_.ToString(), type.ToString()));
  }
}

auto operator==(const Value &lhs, const Value &rhs) -> bool {
  if (lhs.is_null_ || rhs.is_null_) {
    // Two NULLs are equal *as values* here — this is identity, not SQL three-valued
    // logic. SQL NULL = NULL is handled by the comparison expressions, not by this.
    return lhs.is_null_ && rhs.is_null_ && lhs.type_ == rhs.type_;
  }
  if (lhs.type_ != rhs.type_) {
    return false;
  }
  switch (lhs.type_.GetTypeId()) {
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY:
      return lhs.children_ == rhs.children_;
    default:
      break;
  }
  if (lhs.type_.GetPhysicalType() == PhysicalType::STRING) {
    return lhs.string_value_ == rhs.string_value_;
  }
  switch (lhs.type_.GetPhysicalType()) {
    case PhysicalType::FLOAT:
      return lhs.value_.float_ == rhs.value_.float_;
    case PhysicalType::DOUBLE:
      return lhs.value_.double_ == rhs.value_.double_;
    default:
      return lhs.GetAs<int64_t>() == rhs.GetAs<int64_t>();
  }
}

auto operator!=(const Value &lhs, const Value &rhs) -> bool { return !(lhs == rhs); }

}  // namespace bumblebee
