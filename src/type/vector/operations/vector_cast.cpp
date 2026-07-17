//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_cast.cpp
//
// Identification: src/type/vector/operations/vector_cast.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cmath>
#include <cstdlib>
#include <string>

#include "common/exception.h"
#include "common/macros.h"
#include "common/numeric_utils.h"
#include "type/null_value.h"
#include "type/vector/operations/unary_execution.h"
#include "type/vector/operator/cast_operators.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

namespace {

/** The state a cast loop threads through: where to put strings, and where to report errors. */
struct VectorTryCastData {
  VectorTryCastData(Vector &result, std::string *error_message) : result_(result), error_message_(error_message) {}

  Vector &result_;
  std::string *error_message_;
  bool all_converted_{true};
};

/** The state a DECIMAL cast loop needs: the width and the scale to move to. */
struct DecimalCastInput {
  DecimalCastInput(Vector &result, int width, int scale) : result_(result), width_(width), scale_(scale) {}

  Vector &result_;
  int width_;
  int scale_;
  bool all_converted_{true};
};

/** @brief Record a failed conversion and yield the physical NULL fill for the row. */
struct HandleVectorCastError {
  template <class RESULT_TYPE>
  static auto Operation(const std::string &error_message, std::string *error_message_ptr, bool &all_converted)
      -> RESULT_TYPE {
    if (error_message_ptr != nullptr) {
      *error_message_ptr = error_message;
    }
    all_converted = false;
    return NullValue<RESULT_TYPE>();
  }
};

/** @brief Run a checked cast functor, turning a failure into a NULL plus an error. */
template <class OP>
struct VectorTryCastOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, idx_t idx, void *dataptr) -> RESULT_TYPE {
    RESULT_TYPE output;
    auto *data = reinterpret_cast<VectorTryCastData *>(dataptr);
    if (OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, output)) {
      return output;
    }
    // A failed conversion is a NULL, not a silently-wrong value. Clear the row's validity so
    // the result reads as NULL rather than as NullValue<RESULT_TYPE>() — a sentinel that
    // otherwise surfaces as a legitimate-looking 0 / INT_MIN and corrupts downstream reads.
    data->result_.SetInvalid(idx);
    return HandleVectorCastError::Operation<RESULT_TYPE>("error during conversion!", data->error_message_,
                                                         data->all_converted_);
  }
};

/** @brief Run a to-string cast functor, handing it the result Vector so it can use its heap. */
template <class OP>
struct VectorStringCastOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, void *dataptr) -> RESULT_TYPE {
    auto *result = reinterpret_cast<VectorTryCastData *>(dataptr);
    return OP::template Operation<INPUT_TYPE>(input, result->result_);
  }
};

/** @brief Render a DECIMAL row as a string. */
struct StringCastFromDecimalOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, void *dataptr) -> RESULT_TYPE {
    auto *data = reinterpret_cast<DecimalCastInput *>(dataptr);
    return StringTryCastFromDecimal::Operation<INPUT_TYPE>(input, static_cast<uint8_t>(data->width_),
                                                           static_cast<uint8_t>(data->scale_), data->result_);
  }
};

/**
 * @brief Rescale a value into a DECIMAL's backing integer.
 *
 * `scale_` is the DIFFERENCE between the target scale and the source scale, so a positive
 * value scales up and a negative one scales down. Rounding is to nearest, not truncation:
 * casting 1.999 to DECIMAL(x, 2) gives 2.00.
 */
struct DecimalCastOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, void *dataptr) -> RESULT_TYPE {
    auto *data = reinterpret_cast<DecimalCastInput *>(dataptr);
    if (data->scale_ < 0) {
      return static_cast<RESULT_TYPE>(std::llround(static_cast<long double>(input) /
                                                   static_cast<long double>(
                                                       NumericHelper::POWERS_OF_TEN[std::abs(data->scale_)])));
    }
    if (data->scale_ > 0) {
      return static_cast<RESULT_TYPE>(
          std::llround(static_cast<long double>(input) *
                       static_cast<long double>(NumericHelper::POWERS_OF_TEN[data->scale_])));
    }
    return static_cast<RESULT_TYPE>(input);
  }
};

/** @brief Turn a DECIMAL's backing integer back into a real number by dividing out the scale. */
struct DecimalToFloatCastOp {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, void *dataptr) -> RESULT_TYPE {
    auto *data = reinterpret_cast<DecimalCastInput *>(dataptr);
    if (data->scale_ == 0) {
      return static_cast<RESULT_TYPE>(input);
    }
    if (data->scale_ > 0) {
      return static_cast<RESULT_TYPE>(static_cast<long double>(input) /
                                      static_cast<long double>(NumericHelper::POWERS_OF_TEN[data->scale_]));
    }
    return static_cast<RESULT_TYPE>(static_cast<long double>(input) *
                                    static_cast<long double>(NumericHelper::POWERS_OF_TEN[-data->scale_]));
  }
};

template <class SRC, class DST, class OP>
auto VectorCastLoop(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  VectorTryCastData input(result, error_message);
  // The index-aware variant so a per-row failure can mark that row NULL in the result.
  UnaryExecution::GenericExecuteWithIndex<SRC, DST, VectorTryCastOperator<OP>>(source, result, count, &input);
  return input.all_converted_;
}

template <class SRC, class DST, class OP>
auto VectorStringCastLoop(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  VectorTryCastData input(result, error_message);
  UnaryExecution::GenericExecute<SRC, DST, VectorStringCastOperator<OP>>(source, result, count, &input);
  return input.all_converted_;
}

template <class SRC, class DST>
auto VectorDecimalCastLoop(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  (void)error_message;
  int source_scale = source.GetLogicalTypeId() == LogicalTypeId::DECIMAL
                         ? source.GetLogicalType().GetDecimalData().scale_
                         : 0;
  int scale = result.GetLogicalType().GetDecimalData().scale_ - source_scale;
  DecimalCastInput input(result, result.GetLogicalType().GetDecimalData().width_, scale);
  UnaryExecution::GenericExecute<SRC, DST, DecimalCastOperator>(source, result, count, &input);
  return input.all_converted_;
}

/** @brief Cast a numeric source to whatever `result` is typed as. */
template <class SRC>
auto NumericCastSwitch(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  switch (result.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      return VectorCastLoop<SRC, int8_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::SMALLINT:
      return VectorCastLoop<SRC, int16_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::INTEGER:
      return VectorCastLoop<SRC, int32_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
      return VectorCastLoop<SRC, uint8_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::USMALLINT:
      return VectorCastLoop<SRC, uint16_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::UINTEGER:
      return VectorCastLoop<SRC, uint32_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      return VectorCastLoop<SRC, uint64_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::BIGINT:
      return VectorCastLoop<SRC, int64_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::FLOAT:
      return VectorCastLoop<SRC, float, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::DOUBLE:
      return VectorCastLoop<SRC, double, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::STRING:
      return VectorStringCastLoop<SRC, string_t, StringTryCast>(source, result, count, error_message);
    case LogicalTypeId::DATE:
      // A DATE is physically an int32: the number of days since the epoch. A plain numeric
      // value (a constant folded out of file metadata, say) maps straight onto it.
      return VectorCastLoop<SRC, int32_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::TIMESTAMP:
      // A TIMESTAMP is physically an int64: microseconds since the epoch.
      return VectorCastLoop<SRC, int64_t, NumericTryCast>(source, result, count, error_message);
    case LogicalTypeId::DECIMAL: {
      switch (result.GetType()) {
        case PhysicalType::SMALLINT:
          return VectorDecimalCastLoop<SRC, int16_t>(source, result, count, error_message);
        case PhysicalType::INTEGER:
          return VectorDecimalCastLoop<SRC, int32_t>(source, result, count, error_message);
        case PhysicalType::BIGINT:
          return VectorDecimalCastLoop<SRC, int64_t>(source, result, count, error_message);
        default:
          throw NotImplementedException(
              fmt::format("cast: unsupported DECIMAL backing type {}", LogicalType::NameOf(result.GetType())));
      }
    }
    default:
      throw NotImplementedException(
          fmt::format("cast: unsupported target type {}", LogicalType::NameOf(result.GetLogicalTypeId())));
  }
}

/** @brief Parse a STRING source into whatever `result` is typed as. */
auto StringCastSwitch(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  switch (result.GetType()) {
    case PhysicalType::TINYINT:
      return VectorCastLoop<string_t, int8_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::SMALLINT:
      return VectorCastLoop<string_t, int16_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::INTEGER:
      return VectorCastLoop<string_t, int32_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::UTINYINT:
      return VectorCastLoop<string_t, uint8_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::USMALLINT:
      return VectorCastLoop<string_t, uint16_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::UINTEGER:
      return VectorCastLoop<string_t, uint32_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::UBIGINT:
      return VectorCastLoop<string_t, uint64_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::BIGINT:
      return VectorCastLoop<string_t, int64_t, TryIntegerCast>(source, result, count, error_message);
    case PhysicalType::FLOAT:
      return VectorCastLoop<string_t, float, TryDoubleCast>(source, result, count, error_message);
    case PhysicalType::DOUBLE:
      return VectorCastLoop<string_t, double, TryDoubleCast>(source, result, count, error_message);
    default:
      throw NotImplementedException(
          fmt::format("cast: cannot cast a STRING to {}", LogicalType::NameOf(result.GetType())));
  }
}

/** @brief Cast a DECIMAL source to whatever `result` is typed as. */
template <class INPUT_TYPE>
auto DecimalCastSwitch(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  BUMBLEBEE_ASSERT(source.GetLogicalTypeId() == LogicalTypeId::DECIMAL, "the source must be a DECIMAL");

  switch (result.GetLogicalTypeId()) {
    case LogicalTypeId::STRING: {
      const auto &decimal_data = source.GetLogicalType().GetDecimalData();
      DecimalCastInput input(result, decimal_data.width_, decimal_data.scale_);
      UnaryExecution::GenericExecute<INPUT_TYPE, string_t, StringCastFromDecimalOperator>(source, result, count,
                                                                                          &input);
      return true;
    }
    case LogicalTypeId::DECIMAL: {
      switch (result.GetType()) {
        case PhysicalType::SMALLINT:
          return VectorDecimalCastLoop<INPUT_TYPE, int16_t>(source, result, count, error_message);
        case PhysicalType::INTEGER:
          return VectorDecimalCastLoop<INPUT_TYPE, int32_t>(source, result, count, error_message);
        case PhysicalType::BIGINT:
          return VectorDecimalCastLoop<INPUT_TYPE, int64_t>(source, result, count, error_message);
        default:
          throw NotImplementedException(
              fmt::format("cast: unsupported DECIMAL backing type {}", LogicalType::NameOf(result.GetType())));
      }
    }
    case LogicalTypeId::FLOAT: {
      const auto &decimal_data = source.GetLogicalType().GetDecimalData();
      DecimalCastInput input(result, decimal_data.width_, decimal_data.scale_);
      UnaryExecution::GenericExecute<INPUT_TYPE, float, DecimalToFloatCastOp>(source, result, count, &input);
      return true;
    }
    case LogicalTypeId::DOUBLE: {
      const auto &decimal_data = source.GetLogicalType().GetDecimalData();
      DecimalCastInput input(result, decimal_data.width_, decimal_data.scale_);
      UnaryExecution::GenericExecute<INPUT_TYPE, double, DecimalToFloatCastOp>(source, result, count, &input);
      return true;
    }
    default:
      throw NotImplementedException(
          fmt::format("cast: cannot cast a DECIMAL to {}", LogicalType::NameOf(result.GetLogicalTypeId())));
  }
}

/** @brief Render a DATE row as a string. */
struct StringCastFromDateOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, void *dataptr) -> RESULT_TYPE {
    auto *data = reinterpret_cast<VectorTryCastData *>(dataptr);
    return StringCastFromDate::Operation<INPUT_TYPE>(input, data->result_);
  }
};

auto DateCastSwitch(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  VectorTryCastData input(result, error_message);
  if (result.GetLogicalTypeId() == LogicalTypeId::STRING) {
    UnaryExecution::GenericExecute<int32_t, string_t, StringCastFromDateOperator>(source, result, count, &input);
    return true;
  }
  throw NotImplementedException(
      fmt::format("cast: cannot cast a DATE to {}", LogicalType::NameOf(result.GetLogicalTypeId())));
}

/** @brief Render a TIMESTAMP row as a string. */
struct StringCastFromTimestampOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static auto Operation(INPUT_TYPE input, void *dataptr) -> RESULT_TYPE {
    auto *data = reinterpret_cast<VectorTryCastData *>(dataptr);
    return StringCastFromTimestamp::Operation<INPUT_TYPE>(input, data->result_);
  }
};

auto TimestampCastSwitch(Vector &source, Vector &result, idx_t count, std::string *error_message) -> bool {
  VectorTryCastData input(result, error_message);
  if (result.GetLogicalTypeId() == LogicalTypeId::STRING) {
    UnaryExecution::GenericExecute<int64_t, string_t, StringCastFromTimestampOperator>(source, result, count, &input);
    return true;
  }
  throw NotImplementedException(
      fmt::format("cast: cannot cast a TIMESTAMP to {}", LogicalType::NameOf(result.GetLogicalTypeId())));
}

}  // namespace

void VectorOperations::Cast(Vector &source, Vector &target, idx_t source_count) {
  if (source.GetLogicalType() != target.GetLogicalType()) {
    TryCast(source, target, source_count, nullptr);
  } else {
    // Same type: nothing to convert, just move the bytes.
    Copy(source, target, source_count, 0, 0);
  }
}

auto VectorOperations::TryCast(Vector &source, Vector &target, idx_t source_count, std::string *error_message) -> bool {
  BUMBLEBEE_ASSERT(source.GetLogicalType() != target.GetLogicalType(), "TryCast: the types are already the same");

  switch (source.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      return NumericCastSwitch<int8_t>(source, target, source_count, error_message);
    case LogicalTypeId::SMALLINT:
      return NumericCastSwitch<int16_t>(source, target, source_count, error_message);
    case LogicalTypeId::INTEGER:
      return NumericCastSwitch<int32_t>(source, target, source_count, error_message);
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::UTINYINT:
      return NumericCastSwitch<uint8_t>(source, target, source_count, error_message);
    case LogicalTypeId::USMALLINT:
      return NumericCastSwitch<uint16_t>(source, target, source_count, error_message);
    case LogicalTypeId::UINTEGER:
      return NumericCastSwitch<uint32_t>(source, target, source_count, error_message);
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      return NumericCastSwitch<uint64_t>(source, target, source_count, error_message);
    case LogicalTypeId::BIGINT:
      return NumericCastSwitch<int64_t>(source, target, source_count, error_message);
    case LogicalTypeId::FLOAT:
      return NumericCastSwitch<float>(source, target, source_count, error_message);
    case LogicalTypeId::DOUBLE:
      return NumericCastSwitch<double>(source, target, source_count, error_message);
    case LogicalTypeId::STRING:
      return StringCastSwitch(source, target, source_count, error_message);
    case LogicalTypeId::DECIMAL: {
      switch (source.GetType()) {
        case PhysicalType::SMALLINT:
          return DecimalCastSwitch<int16_t>(source, target, source_count, error_message);
        case PhysicalType::INTEGER:
          return DecimalCastSwitch<int32_t>(source, target, source_count, error_message);
        case PhysicalType::BIGINT:
          return DecimalCastSwitch<int64_t>(source, target, source_count, error_message);
        default:
          throw NotImplementedException(
              fmt::format("cast: unsupported DECIMAL backing type {}", LogicalType::NameOf(source.GetType())));
      }
    }
    case LogicalTypeId::DATE:
      return DateCastSwitch(source, target, source_count, error_message);
    case LogicalTypeId::TIMESTAMP:
      return TimestampCastSwitch(source, target, source_count, error_message);
    default:
      throw NotImplementedException(
          fmt::format("cast: unsupported source type {}", LogicalType::NameOf(source.GetLogicalTypeId())));
  }
}

}  // namespace bumblebee
