//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// logical_type.cpp
//
// Identification: src/type/logical_type.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/logical_type.h"

#include <algorithm>
#include <utility>

#include "common/exception.h"
#include "common/macros.h"
#include "common/util/string_util.h"
#include "type/bumble_string.h"
#include "type/list_entry.h"

namespace bumblebee {

// ---------------------------------------------------------------------------
// Extra data
// ---------------------------------------------------------------------------

auto DecimalTypeData::Equals(const LogicalTypeExtraData &other) const -> bool {
  const auto *o = dynamic_cast<const DecimalTypeData *>(&other);
  return o != nullptr && width_ == o->width_ && scale_ == o->scale_;
}

ListTypeData::ListTypeData(const LogicalType &child_type, idx_t size)
    : child_type_(std::make_shared<LogicalType>(child_type)), size_(size) {}

auto ListTypeData::Equals(const LogicalTypeExtraData &other) const -> bool {
  const auto *o = dynamic_cast<const ListTypeData *>(&other);
  return o != nullptr && size_ == o->size_ && *child_type_ == *o->child_type_;
}

// ---------------------------------------------------------------------------
// Enum helpers
// ---------------------------------------------------------------------------

auto LogicalType::PhysicalTypeOf(LogicalTypeId type) -> PhysicalType {
  switch (type) {
    case LogicalTypeId::TINYINT:
      return PhysicalType::TINYINT;
    case LogicalTypeId::SMALLINT:
      return PhysicalType::SMALLINT;
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
      return PhysicalType::INTEGER;
    // A DECIMAL with no declared width falls back to the widest backing type;
    // Decimal() narrows it once the precision is known.
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::DECIMAL:
      return PhysicalType::BIGINT;
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
      return PhysicalType::UTINYINT;
    case LogicalTypeId::USMALLINT:
      return PhysicalType::USMALLINT;
    case LogicalTypeId::UINTEGER:
      return PhysicalType::UINTEGER;
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
      return PhysicalType::UBIGINT;
    case LogicalTypeId::FLOAT:
      return PhysicalType::FLOAT;
    case LogicalTypeId::DOUBLE:
      return PhysicalType::DOUBLE;
    case LogicalTypeId::STRING:
      return PhysicalType::STRING;
    case LogicalTypeId::STRUCT:
      return PhysicalType::STRUCT;
    case LogicalTypeId::LIST:
      return PhysicalType::LIST;
    case LogicalTypeId::ARRAY:
      return PhysicalType::ARRAY;
    default:
      return PhysicalType::UNKNOWN;
  }
}

auto LogicalType::LogicalTypeIdOf(PhysicalType type) -> LogicalTypeId {
  switch (type) {
    case PhysicalType::TINYINT:
      return LogicalTypeId::TINYINT;
    case PhysicalType::SMALLINT:
      return LogicalTypeId::SMALLINT;
    case PhysicalType::INTEGER:
      return LogicalTypeId::INTEGER;
    case PhysicalType::BIGINT:
      return LogicalTypeId::BIGINT;
    case PhysicalType::UTINYINT:
      return LogicalTypeId::UTINYINT;
    case PhysicalType::USMALLINT:
      return LogicalTypeId::USMALLINT;
    case PhysicalType::UINTEGER:
      return LogicalTypeId::UINTEGER;
    case PhysicalType::UBIGINT:
      return LogicalTypeId::UBIGINT;
    case PhysicalType::FLOAT:
      return LogicalTypeId::FLOAT;
    case PhysicalType::DOUBLE:
      return LogicalTypeId::DOUBLE;
    case PhysicalType::STRING:
      return LogicalTypeId::STRING;
    case PhysicalType::STRUCT:
      return LogicalTypeId::STRUCT;
    case PhysicalType::LIST:
      return LogicalTypeId::LIST;
    case PhysicalType::ARRAY:
      return LogicalTypeId::ARRAY;
    default:
      return LogicalTypeId::UNKNOWN;
  }
}

auto LogicalType::SizeOf(PhysicalType type) -> idx_t {
  switch (type) {
    case PhysicalType::TINYINT:
    case PhysicalType::UTINYINT:
      return 1;
    case PhysicalType::SMALLINT:
    case PhysicalType::USMALLINT:
      return 2;
    case PhysicalType::INTEGER:
    case PhysicalType::UINTEGER:
    case PhysicalType::FLOAT:
      return 4;
    case PhysicalType::BIGINT:
    case PhysicalType::UBIGINT:
    case PhysicalType::DOUBLE:
      return 8;
    // A string's *inline* size is the size of the handle, not of the bytes it points
    // at: a Vector of STRING stores one BumbleString per row and keeps the payload in
    // a StringHeap off to the side. This is why SizeOf and IsConstantSize disagree
    // here — IsConstantSize(STRING) is false because the *payload* is variable, but
    // the vector still needs a fixed stride to allocate.
    case PhysicalType::STRING:
      return sizeof(BumbleString);
    // A LIST row stores a (offset, length) pair inline and keeps the elements in the
    // child Vector, exactly like a STRING stores a handle and keeps the bytes in a heap.
    case PhysicalType::LIST:
      return sizeof(ListEntry);
    // An ARRAY has NO inline payload at all: row i is the child slice
    // [i * array_size, (i + 1) * array_size), so there is nothing to store per row and
    // nothing to stride over. Zero is the honest answer, and it is what makes the generic
    // `capacity * SizeOf(type)` allocation in VectorDataMngr::CreateStandardVector produce
    // an empty (but valid) buffer for an ARRAY Vector; Vector::Initialize then allocates
    // the child, which is where an ARRAY's rows actually live.
    case PhysicalType::ARRAY:
      return 0;
    // An UNKNOWN vector is the untyped NULL literal: every row is NULL, so only the validity
    // mask carries information. One byte per row is a minimal defensive fill, which is what
    // lets `SELECT NULL` (and a NULL cell in VALUES) materialize before a cast resolves it.
    case PhysicalType::UNKNOWN:
      return 1;
    default:
      throw NotImplementedException(fmt::format("{} has no inline size", NameOf(type)));
  }
}

auto LogicalType::IsConstantSize(PhysicalType type) -> bool {
  switch (type) {
    case PhysicalType::STRING:
    case PhysicalType::STRUCT:
    case PhysicalType::LIST:
    case PhysicalType::ARRAY:
      return false;
    default:
      // Includes UNKNOWN: an all-NULL vector strides over its 1-byte defensive fill.
      return true;
  }
}

auto LogicalType::NameOf(PhysicalType type) -> std::string {
  switch (type) {
    case PhysicalType::TINYINT:
      return "TINYINT";
    case PhysicalType::SMALLINT:
      return "SMALLINT";
    case PhysicalType::INTEGER:
      return "INTEGER";
    case PhysicalType::BIGINT:
      return "BIGINT";
    case PhysicalType::UTINYINT:
      return "UTINYINT";
    case PhysicalType::USMALLINT:
      return "USMALLINT";
    case PhysicalType::UINTEGER:
      return "UINTEGER";
    case PhysicalType::UBIGINT:
      return "UBIGINT";
    case PhysicalType::FLOAT:
      return "FLOAT";
    case PhysicalType::DOUBLE:
      return "DOUBLE";
    case PhysicalType::STRING:
      return "STRING";
    case PhysicalType::STRUCT:
      return "STRUCT";
    case PhysicalType::LIST:
      return "LIST";
    case PhysicalType::ARRAY:
      return "ARRAY";
    default:
      return "UNKNOWN";
  }
}

auto LogicalType::NameOf(LogicalTypeId type) -> std::string {
  switch (type) {
    case LogicalTypeId::BOOLEAN:
      return "BOOLEAN";
    case LogicalTypeId::TINYINT:
      return "TINYINT";
    case LogicalTypeId::SMALLINT:
      return "SMALLINT";
    case LogicalTypeId::INTEGER:
      return "INTEGER";
    case LogicalTypeId::BIGINT:
      return "BIGINT";
    case LogicalTypeId::UTINYINT:
      return "UTINYINT";
    case LogicalTypeId::USMALLINT:
      return "USMALLINT";
    case LogicalTypeId::UINTEGER:
      return "UINTEGER";
    case LogicalTypeId::UBIGINT:
      return "UBIGINT";
    case LogicalTypeId::HASH:
      return "HASH";
    case LogicalTypeId::ADDRESS:
      return "ADDRESS";
    case LogicalTypeId::FLOAT:
      return "FLOAT";
    case LogicalTypeId::DOUBLE:
      return "DOUBLE";
    case LogicalTypeId::DECIMAL:
      return "DECIMAL";
    case LogicalTypeId::STRING:
      return "VARCHAR";
    case LogicalTypeId::STRUCT:
      return "STRUCT";
    case LogicalTypeId::LIST:
      return "LIST";
    case LogicalTypeId::ARRAY:
      return "ARRAY";
    case LogicalTypeId::DATE:
      return "DATE";
    case LogicalTypeId::TIMESTAMP:
      return "TIMESTAMP";
    default:
      return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LogicalType::LogicalType(LogicalTypeId type) : ctype_(PhysicalTypeOf(type)), type_(type) {
  if (type == LogicalTypeId::DECIMAL) {
    data_ = std::make_shared<DecimalTypeData>(MAX_DECIMAL_WIDTH_INT64, 0);
  }
}

LogicalType::LogicalType(PhysicalType type) : ctype_(type), type_(LogicalTypeIdOf(type)) {}

auto LogicalType::Decimal(int width, int scale) -> LogicalType {
  LogicalType type(LogicalTypeId::DECIMAL);
  if (width <= MAX_DECIMAL_WIDTH_INT16) {
    type.ctype_ = PhysicalType::SMALLINT;
  } else if (width <= MAX_DECIMAL_WIDTH_INT32) {
    type.ctype_ = PhysicalType::INTEGER;
  } else if (width <= MAX_DECIMAL_WIDTH_INT64) {
    type.ctype_ = PhysicalType::BIGINT;
  } else {
    throw NotImplementedException(
        fmt::format("DECIMAL precision {} exceeds the maximum of {}", width, MAX_DECIMAL_WIDTH_INT64));
  }
  type.data_ = std::make_shared<DecimalTypeData>(width, scale);
  return type;
}

auto LogicalType::List(const LogicalType &child_type) -> LogicalType {
  LogicalType type(LogicalTypeId::LIST);
  type.data_ = std::make_shared<ListTypeData>(child_type, 0);
  return type;
}

auto LogicalType::Array(const LogicalType &child_type, idx_t size) -> LogicalType {
  LogicalType type(LogicalTypeId::ARRAY);
  type.data_ = std::make_shared<ListTypeData>(child_type, size);
  return type;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

auto LogicalType::GetDecimalData() const -> const DecimalTypeData & {
  BUMBLEBEE_ASSERT(type_ == LogicalTypeId::DECIMAL, "not a DECIMAL type");
  return static_cast<const DecimalTypeData &>(*data_);
}

auto LogicalType::GetListData() const -> const ListTypeData & {
  BUMBLEBEE_ASSERT(type_ == LogicalTypeId::LIST || type_ == LogicalTypeId::ARRAY,
                   "not a LIST or ARRAY type");
  return static_cast<const ListTypeData &>(*data_);
}

auto LogicalType::GetChildType() const -> const LogicalType & { return *GetListData().child_type_; }

auto LogicalType::ToString() const -> std::string {
  switch (type_) {
    case LogicalTypeId::DECIMAL: {
      const auto &decimal = GetDecimalData();
      return fmt::format("DECIMAL({},{})", decimal.width_, decimal.scale_);
    }
    case LogicalTypeId::LIST:
      return fmt::format("{}[]", GetChildType().ToString());
    case LogicalTypeId::ARRAY:
      return fmt::format("{}[{}]", GetChildType().ToString(), GetListData().size_);
    default:
      return NameOf(type_);
  }
}

auto operator==(const LogicalType &lhs, const LogicalType &rhs) -> bool {
  if (lhs.ctype_ != rhs.ctype_ || lhs.type_ != rhs.type_) {
    return false;
  }
  if (lhs.data_ == rhs.data_) {
    return true;
  }
  if (lhs.data_ == nullptr || rhs.data_ == nullptr) {
    return false;
  }
  return lhs.data_->Equals(*rhs.data_);
}

auto operator!=(const LogicalType &lhs, const LogicalType &rhs) -> bool { return !(lhs == rhs); }

// ---------------------------------------------------------------------------
// Type resolution
// ---------------------------------------------------------------------------

auto LogicalType::CommonType(const LogicalType &lhs, const LogicalType &rhs) -> LogicalType {
  if (lhs == rhs) {
    return lhs;
  }
  if (lhs.type_ == LogicalTypeId::UNKNOWN) {
    return rhs;
  }
  if (rhs.type_ == LogicalTypeId::UNKNOWN) {
    return lhs;
  }

  const auto has = [&](LogicalTypeId id) { return lhs.type_ == id || rhs.type_ == id; };

  // A temporal type absorbs a string: '1995-01-01' compared to a DATE column coerces the LITERAL
  // to DATE (one strict cast) — the other direction would format every DATE of the column into a
  // string per row just to compare lexicographically.
  if (has(LogicalTypeId::STRING)) {
    if (has(LogicalTypeId::DATE)) {
      return LogicalTypeId::DATE;
    }
    if (has(LogicalTypeId::TIMESTAMP)) {
      return LogicalTypeId::TIMESTAMP;
    }
    // Otherwise the string absorbs: comparing a number against a string casts the number.
    return LogicalTypeId::STRING;
  }
  // Floating point absorbs the integers and DECIMAL.
  if (has(LogicalTypeId::DOUBLE)) {
    return LogicalTypeId::DOUBLE;
  }
  if (has(LogicalTypeId::FLOAT)) {
    return LogicalTypeId::FLOAT;
  }
  if (has(LogicalTypeId::DECIMAL)) {
    const auto scale_of = [](const LogicalType &t) {
      return t.type_ == LogicalTypeId::DECIMAL ? t.GetDecimalData().scale_ : 0;
    };
    return Decimal(MAX_DECIMAL_WIDTH_INT64, std::max(scale_of(lhs), scale_of(rhs)));
  }
  // Otherwise widen to whichever side is physically bigger.
  return SizeOf(lhs.ctype_) >= SizeOf(rhs.ctype_) ? lhs : rhs;
}

auto LogicalType::FromString(const std::string &type_name) -> LogicalType {
  const auto name = StringUtil::Lower(type_name);

  if (name == "bool" || name == "boolean") {
    return LogicalTypeId::BOOLEAN;
  }
  if (name == "int1" || name == "tinyint") {
    return LogicalTypeId::TINYINT;
  }
  if (name == "int2" || name == "smallint" || name == "short") {
    return LogicalTypeId::SMALLINT;
  }
  if (name == "int4" || name == "int" || name == "integer" || name == "signed") {
    return LogicalTypeId::INTEGER;
  }
  if (name == "int8" || name == "bigint" || name == "long") {
    return LogicalTypeId::BIGINT;
  }
  if (name == "float4" || name == "float" || name == "real") {
    return LogicalTypeId::FLOAT;
  }
  if (name == "float8" || name == "double" || name == "numeric") {
    return LogicalTypeId::DOUBLE;
  }
  if (name == "decimal") {
    return LogicalTypeId::DECIMAL;
  }
  if (name == "varchar" || name == "text" || name == "char" || name == "bpchar" || name == "string") {
    return LogicalTypeId::STRING;
  }
  if (name == "date") {
    return LogicalTypeId::DATE;
  }
  if (name == "timestamp" || name == "datetime") {
    return LogicalTypeId::TIMESTAMP;
  }
  return LogicalTypeId::UNKNOWN;
}

}  // namespace bumblebee
