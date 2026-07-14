//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_arith.cpp
//
// Identification: src/type/vector/operations/vector_arith.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cstdlib>

#include "common/exception.h"
#include "common/helper.h"
#include "common/limits.h"
#include "common/macros.h"
#include "common/numeric_utils.h"
#include "type/vector/operations/binary_execution.h"
#include "type/vector/operations/unary_execution.h"
#include "type/vector/operator/arith_operators.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

namespace {

/** @brief `-value`, produced in the result's type. */
struct ArithNegate {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static inline auto Operation(INPUT_TYPE value) -> RESULT_TYPE {
    return static_cast<RESULT_TYPE>(value) * -1;
  }
};

template <class INPUT_TYPE>
void TemplatedExecuteNegateSwitchResult(Vector &input, Vector &result, idx_t count) {
  switch (result.GetType()) {
    case PhysicalType::TINYINT:
      UnaryExecution::Execute<INPUT_TYPE, int8_t, ArithNegate>(input, result, count);
      break;
    case PhysicalType::SMALLINT:
      UnaryExecution::Execute<INPUT_TYPE, int16_t, ArithNegate>(input, result, count);
      break;
    case PhysicalType::INTEGER:
      UnaryExecution::Execute<INPUT_TYPE, int32_t, ArithNegate>(input, result, count);
      break;
    case PhysicalType::BIGINT:
      UnaryExecution::Execute<INPUT_TYPE, int64_t, ArithNegate>(input, result, count);
      break;
    case PhysicalType::FLOAT:
      UnaryExecution::Execute<INPUT_TYPE, float, ArithNegate>(input, result, count);
      break;
    case PhysicalType::DOUBLE:
      UnaryExecution::Execute<INPUT_TYPE, double, ArithNegate>(input, result, count);
      break;
    default:
      throw NotImplementedException(
          fmt::format("negate: unsupported result type {}", LogicalType::NameOf(result.GetType())));
  }
}

/** @brief The fast path: both inputs and the result share one logical type. */
template <class OP>
void TemplatedExecuteOperationSwitchEqualType(Vector &left, Vector &right, Vector &result, idx_t count) {
  BUMBLEBEE_ASSERT(left.GetLogicalType() == right.GetLogicalType() && left.GetLogicalType() == result.GetLogicalType(),
                   "the equal-type path needs all three types to match");
  switch (left.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      BinaryExecution::Execute<int8_t, int8_t, int8_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::SMALLINT:
      BinaryExecution::Execute<int16_t, int16_t, int16_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::DATE:
    case LogicalTypeId::INTEGER:
      BinaryExecution::Execute<int32_t, int32_t, int32_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::DECIMAL:
    case LogicalTypeId::BIGINT:
      BinaryExecution::Execute<int64_t, int64_t, int64_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::UTINYINT:
      BinaryExecution::Execute<uint8_t, uint8_t, uint8_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::USMALLINT:
      BinaryExecution::Execute<uint16_t, uint16_t, uint16_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::UINTEGER:
      BinaryExecution::Execute<uint32_t, uint32_t, uint32_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      BinaryExecution::Execute<uint64_t, uint64_t, uint64_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::FLOAT:
      BinaryExecution::Execute<float, float, float, OP>(left, right, result, count);
      break;
    case LogicalTypeId::DOUBLE:
      BinaryExecution::Execute<double, double, double, OP>(left, right, result, count);
      break;
    case LogicalTypeId::STRING:
      BinaryExecution::Execute<string_t, string_t, string_t, OP>(left, right, result, count);
      break;
    default:
      throw NotImplementedException(
          fmt::format("arithmetic: unsupported type {}", LogicalType::NameOf(left.GetLogicalTypeId())));
  }
}

// NOTE: a DECIMAL(18,2) stored as a BIGINT holds 1.00 as the integer 100. Arithmetic on
// two DECIMALs therefore cannot just add the raw integers unless the scales agree, and the
// result has its own scale to hit. The DecimalCommonCast functors below do the rescaling
// per row, with the scales carried in through `dataptr`.

/** @brief Promote both operands to RESULT_TYPE, then apply OP. */
template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
struct ArithCommonCast {
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right) -> RESULT_TYPE {
    return OP::Operation(static_cast<RESULT_TYPE>(left), static_cast<RESULT_TYPE>(right));
  }
};

/** The scales of the two DECIMAL operands and of the DECIMAL result. */
struct DecimalCommonCastInput {
  DecimalCommonCastInput(int left_scale, int right_scale, int result_scale)
      : left_scale_(left_scale), right_scale_(right_scale), result_scale_(result_scale) {}

  int left_scale_{0};
  int right_scale_{0};
  int result_scale_{0};
};

/** The scale of the single DECIMAL operand in a mixed DECIMAL / floating-point operation. */
struct MixedDecimalFloatInput {
  explicit MixedDecimalFloatInput(int decimal_scale) : decimal_scale_(decimal_scale) {}
  int decimal_scale_{0};
};

/** @brief DECIMAL (left) against a float (right): unscale the DECIMAL into a real number. */
template <class LEFT_DECIMAL_INT, class RIGHT_FLOAT, class RESULT_FLOAT, class OP>
struct DecimalFloatCast {
  static inline auto Operation(LEFT_DECIMAL_INT left, RIGHT_FLOAT right, idx_t idx, void *dataptr) -> RESULT_FLOAT {
    (void)idx;
    auto *input = reinterpret_cast<MixedDecimalFloatInput *>(dataptr);
    auto left_as_float = static_cast<RESULT_FLOAT>(left) /
                         static_cast<RESULT_FLOAT>(NumericHelper::POWERS_OF_TEN[input->decimal_scale_]);
    return OP::Operation(left_as_float, static_cast<RESULT_FLOAT>(right));
  }
};

/** @brief A float (left) against a DECIMAL (right). */
template <class LEFT_FLOAT, class RIGHT_DECIMAL_INT, class RESULT_FLOAT, class OP>
struct FloatDecimalCast {
  static inline auto Operation(LEFT_FLOAT left, RIGHT_DECIMAL_INT right, idx_t idx, void *dataptr) -> RESULT_FLOAT {
    (void)idx;
    auto *input = reinterpret_cast<MixedDecimalFloatInput *>(dataptr);
    auto right_as_float = static_cast<RESULT_FLOAT>(right) /
                          static_cast<RESULT_FLOAT>(NumericHelper::POWERS_OF_TEN[input->decimal_scale_]);
    return OP::Operation(static_cast<RESULT_FLOAT>(left), right_as_float);
  }
};

/**
 * @brief The general DECIMAL arithmetic: rescale both operands to the result scale, then OP.
 *
 * This is right for `+` and `-`, where the scales must simply line up. `*` and `/` need
 * different algebra and are specialized below.
 */
template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
struct DecimalCommonCast {
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    auto *input = reinterpret_cast<DecimalCommonCastInput *>(dataptr);
    auto left_result_type = static_cast<RESULT_TYPE>(left);
    auto right_result_type = static_cast<RESULT_TYPE>(right);

    // Scale up by 10^(result - operand) when the result is wider, and down otherwise.
    if (input->result_scale_ >= input->left_scale_) {
      left_result_type *= NumericHelper::POWERS_OF_TEN[input->result_scale_ - input->left_scale_];
    } else {
      left_result_type /= NumericHelper::POWERS_OF_TEN[input->left_scale_ - input->result_scale_];
    }

    if (input->result_scale_ >= input->right_scale_) {
      right_result_type *= NumericHelper::POWERS_OF_TEN[input->result_scale_ - input->right_scale_];
    } else {
      right_result_type /= NumericHelper::POWERS_OF_TEN[input->right_scale_ - input->result_scale_];
    }

    return OP::Operation(left_result_type, right_result_type);
  }
};

/**
 * @brief DECIMAL multiplication.
 *
 * Multiplying the raw integers already adds the scales (raw scale = left + right), so
 * rescaling the operands first — as the generic path does — would double-count. Multiply
 * raw, then bring the product onto the result scale. The product is formed in a 128-bit
 * intermediate so that two 18-digit BIGINT decimals do not overflow on the way.
 */
template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
struct DecimalCommonCast<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, Dot> {
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    auto *in = reinterpret_cast<DecimalCommonCastInput *>(dataptr);

    __int128 raw = static_cast<__int128>(left) * static_cast<__int128>(right);

    int raw_scale = in->left_scale_ + in->right_scale_;
    int target = in->result_scale_;

    if (target > raw_scale) {
      raw *= NumericHelper::POWERS_OF_TEN[target - raw_scale];
    } else if (target < raw_scale) {
      // Truncation toward zero.
      raw /= NumericHelper::POWERS_OF_TEN[raw_scale - target];
    }
    return static_cast<RESULT_TYPE>(raw);
  }
};

/**
 * @brief DECIMAL division.
 *
 * Dividing the raw integers SUBTRACTS the scales, so the numerator has to be pre-scaled by
 * 10^(result + right - left) for the quotient to land on the result scale. Again in 128
 * bits, since that pre-scaling is what overflows.
 */
template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
struct DecimalCommonCast<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, Division> {
  static inline auto Operation(LEFT_TYPE left, RIGHT_TYPE right, idx_t idx, void *dataptr) -> RESULT_TYPE {
    (void)idx;
    auto *in = reinterpret_cast<DecimalCommonCastInput *>(dataptr);

    if (right == 0) {
      // Division by zero: saturate rather than trap.
      return NumericLimits<RESULT_TYPE>::Maximum();
    }

    const int exp = in->result_scale_ + in->right_scale_ - in->left_scale_;

    __int128 num = static_cast<__int128>(left);
    __int128 den = static_cast<__int128>(right);

    if (exp >= 0) {
      num *= NumericHelper::POWERS_OF_TEN[exp];
    } else {
      // A negative exponent would need a division before the division. That loses
      // precision, but it is consistent with the truncating integer arithmetic here.
      num /= NumericHelper::POWERS_OF_TEN[-exp];
    }

    __int128 raw = num / den;  // truncation toward zero
    return static_cast<RESULT_TYPE>(raw);
  }
};

template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
void TemplatedExecuteOperationSwitchDecimalMaxScale(Vector &left, Vector &right, Vector &result, idx_t count) {
  BUMBLEBEE_ASSERT(left.GetLogicalTypeId() == LogicalTypeId::DECIMAL, "the left operand must be a DECIMAL");
  BUMBLEBEE_ASSERT(right.GetLogicalTypeId() == LogicalTypeId::DECIMAL, "the right operand must be a DECIMAL");
  int sl = left.GetLogicalType().GetDecimalData().scale_;
  int sr = right.GetLogicalType().GetDecimalData().scale_;
  int sres = result.GetLogicalType().GetDecimalData().scale_;
  DecimalCommonCastInput input(sl, sr, sres);

  BinaryExecution::GenericExecute<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE,
                                  DecimalCommonCast<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OP>>(left, right, result, count,
                                                                                             &input);
}

template <class LEFT_DECIMAL_INT, class OP>
void TemplatedExecuteOperationDecimalFloat(Vector &left, Vector &right, Vector &result, idx_t count) {
  BUMBLEBEE_ASSERT(left.GetLogicalTypeId() == LogicalTypeId::DECIMAL, "the left operand must be a DECIMAL");
  BUMBLEBEE_ASSERT(result.GetLogicalTypeId() == LogicalTypeId::DOUBLE, "a DECIMAL / float result must be a DOUBLE");
  int scale = left.GetLogicalType().GetDecimalData().scale_;
  MixedDecimalFloatInput input(scale);
  switch (right.GetLogicalTypeId()) {
    case LogicalTypeId::FLOAT:
      BinaryExecution::GenericExecute<LEFT_DECIMAL_INT, float, double,
                                      DecimalFloatCast<LEFT_DECIMAL_INT, float, double, OP>>(left, right, result, count,
                                                                                             &input);
      break;
    case LogicalTypeId::DOUBLE:
      BinaryExecution::GenericExecute<LEFT_DECIMAL_INT, double, double,
                                      DecimalFloatCast<LEFT_DECIMAL_INT, double, double, OP>>(left, right, result,
                                                                                              count, &input);
      break;
    default:
      throw NotImplementedException(fmt::format("arithmetic: unsupported right type {} for a DECIMAL / float operation",
                                                LogicalType::NameOf(right.GetLogicalTypeId())));
  }
}

template <class RIGHT_DECIMAL_INT, class OP>
void TemplatedExecuteOperationFloatDecimal(Vector &left, Vector &right, Vector &result, idx_t count) {
  BUMBLEBEE_ASSERT(right.GetLogicalTypeId() == LogicalTypeId::DECIMAL, "the right operand must be a DECIMAL");
  BUMBLEBEE_ASSERT(result.GetLogicalTypeId() == LogicalTypeId::DOUBLE, "a float / DECIMAL result must be a DOUBLE");
  int scale = right.GetLogicalType().GetDecimalData().scale_;
  MixedDecimalFloatInput input(scale);
  switch (left.GetLogicalTypeId()) {
    case LogicalTypeId::FLOAT:
      BinaryExecution::GenericExecute<float, RIGHT_DECIMAL_INT, double,
                                      FloatDecimalCast<float, RIGHT_DECIMAL_INT, double, OP>>(left, right, result,
                                                                                              count, &input);
      break;
    case LogicalTypeId::DOUBLE:
      BinaryExecution::GenericExecute<double, RIGHT_DECIMAL_INT, double,
                                      FloatDecimalCast<double, RIGHT_DECIMAL_INT, double, OP>>(left, right, result,
                                                                                               count, &input);
      break;
    default:
      throw NotImplementedException(fmt::format("arithmetic: unsupported left type {} for a float / DECIMAL operation",
                                                LogicalType::NameOf(left.GetLogicalTypeId())));
  }
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP>
void TemplatedExecuteOperationSwitchResult(Vector &left, Vector &right, Vector &result, idx_t count) {
  switch (result.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, int8_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, int8_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::SMALLINT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, int16_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, int16_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::DATE:
    case LogicalTypeId::INTEGER:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, int32_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, int32_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::UTINYINT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, uint8_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, uint8_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::USMALLINT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, uint16_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, uint16_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::UINTEGER:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, uint32_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, uint32_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::HASH:
    case LogicalTypeId::UBIGINT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, uint64_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, uint64_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::BIGINT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, int64_t, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, int64_t, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::FLOAT:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, float, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, float, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::DOUBLE:
      BinaryExecution::Execute<LEFT_TYPE, RIGHT_TYPE, double, ArithCommonCast<LEFT_TYPE, RIGHT_TYPE, double, OP>>(
          left, right, result, count);
      break;
    case LogicalTypeId::DECIMAL: {
      switch (result.GetType()) {
        case PhysicalType::SMALLINT:
          TemplatedExecuteOperationSwitchDecimalMaxScale<LEFT_TYPE, RIGHT_TYPE, int16_t, OP>(left, right, result, count);
          break;
        case PhysicalType::INTEGER:
          TemplatedExecuteOperationSwitchDecimalMaxScale<LEFT_TYPE, RIGHT_TYPE, int32_t, OP>(left, right, result, count);
          break;
        case PhysicalType::BIGINT:
          TemplatedExecuteOperationSwitchDecimalMaxScale<LEFT_TYPE, RIGHT_TYPE, int64_t, OP>(left, right, result, count);
          break;
        default:
          throw NotImplementedException(
              fmt::format("arithmetic: unsupported DECIMAL backing type {}", LogicalType::NameOf(result.GetType())));
      }
      break;
    }
    default:
      throw NotImplementedException(
          fmt::format("arithmetic: unsupported result type {}", LogicalType::NameOf(result.GetLogicalTypeId())));
  }
}

template <class LEFT_TYPE, class OP>
void TemplatedExecuteOperationSwitchRight(Vector &left, Vector &right, Vector &result, idx_t count) {
  switch (right.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int8_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::SMALLINT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int16_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::INTEGER:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int32_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::UTINYINT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, uint8_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::USMALLINT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, uint16_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::UINTEGER:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, uint32_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, uint64_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::BIGINT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int64_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::FLOAT:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, float, OP>(left, right, result, count);
      break;
    case LogicalTypeId::DOUBLE:
      TemplatedExecuteOperationSwitchResult<LEFT_TYPE, double, OP>(left, right, result, count);
      break;
    case LogicalTypeId::DECIMAL: {
      const bool left_is_float = left.GetLogicalTypeId() == LogicalTypeId::FLOAT ||
                                 left.GetLogicalTypeId() == LogicalTypeId::DOUBLE;
      switch (right.GetType()) {
        case PhysicalType::SMALLINT:
          left_is_float ? TemplatedExecuteOperationFloatDecimal<int16_t, OP>(left, right, result, count)
                        : TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int16_t, OP>(left, right, result, count);
          break;
        case PhysicalType::INTEGER:
          left_is_float ? TemplatedExecuteOperationFloatDecimal<int32_t, OP>(left, right, result, count)
                        : TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int32_t, OP>(left, right, result, count);
          break;
        case PhysicalType::BIGINT:
          left_is_float ? TemplatedExecuteOperationFloatDecimal<int64_t, OP>(left, right, result, count)
                        : TemplatedExecuteOperationSwitchResult<LEFT_TYPE, int64_t, OP>(left, right, result, count);
          break;
        default:
          throw NotImplementedException(
              fmt::format("arithmetic: unsupported DECIMAL backing type {}", LogicalType::NameOf(right.GetType())));
      }
      break;
    }
    default:
      throw NotImplementedException(
          fmt::format("arithmetic: unsupported right type {}", LogicalType::NameOf(right.GetLogicalTypeId())));
  }
}

/** @brief The slow path: the types differ, so left, right and result must each be dispatched. */
template <class OP>
void TemplatedExecuteOperationSwitchLeft(Vector &left, Vector &right, Vector &result, idx_t count) {
  switch (left.GetLogicalTypeId()) {
    case LogicalTypeId::TINYINT:
      TemplatedExecuteOperationSwitchRight<int8_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::SMALLINT:
      TemplatedExecuteOperationSwitchRight<int16_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::INTEGER:
      TemplatedExecuteOperationSwitchRight<int32_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::UTINYINT:
      TemplatedExecuteOperationSwitchRight<uint8_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::USMALLINT:
      TemplatedExecuteOperationSwitchRight<uint16_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::UINTEGER:
      TemplatedExecuteOperationSwitchRight<uint32_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::HASH:
    case LogicalTypeId::ADDRESS:
    case LogicalTypeId::UBIGINT:
      TemplatedExecuteOperationSwitchRight<uint64_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::BIGINT:
      TemplatedExecuteOperationSwitchRight<int64_t, OP>(left, right, result, count);
      break;
    case LogicalTypeId::FLOAT:
      TemplatedExecuteOperationSwitchRight<float, OP>(left, right, result, count);
      break;
    case LogicalTypeId::DOUBLE:
      TemplatedExecuteOperationSwitchRight<double, OP>(left, right, result, count);
      break;
    case LogicalTypeId::DECIMAL: {
      const bool right_is_float = right.GetLogicalTypeId() == LogicalTypeId::FLOAT ||
                                  right.GetLogicalTypeId() == LogicalTypeId::DOUBLE;
      switch (left.GetType()) {
        case PhysicalType::SMALLINT:
          right_is_float ? TemplatedExecuteOperationDecimalFloat<int16_t, OP>(left, right, result, count)
                         : TemplatedExecuteOperationSwitchRight<int16_t, OP>(left, right, result, count);
          break;
        case PhysicalType::INTEGER:
          right_is_float ? TemplatedExecuteOperationDecimalFloat<int32_t, OP>(left, right, result, count)
                         : TemplatedExecuteOperationSwitchRight<int32_t, OP>(left, right, result, count);
          break;
        case PhysicalType::BIGINT:
          right_is_float ? TemplatedExecuteOperationDecimalFloat<int64_t, OP>(left, right, result, count)
                         : TemplatedExecuteOperationSwitchRight<int64_t, OP>(left, right, result, count);
          break;
        default:
          throw NotImplementedException(
              fmt::format("arithmetic: unsupported DECIMAL backing type {}", LogicalType::NameOf(left.GetType())));
      }
      break;
    }
    default:
      throw NotImplementedException(
          fmt::format("arithmetic: unsupported left type {}", LogicalType::NameOf(left.GetLogicalTypeId())));
  }
}

/** @brief Dispatch an arithmetic op: the equal-type fast path, or the promoting slow path. */
template <class OP>
void ExecuteOperation(Vector &left, Vector &right, Vector &result, idx_t count) {
  if (left.GetLogicalType() == right.GetLogicalType() && left.GetLogicalType() == result.GetLogicalType()) {
    TemplatedExecuteOperationSwitchEqualType<OP>(left, right, result, count);
  } else {
    TemplatedExecuteOperationSwitchLeft<OP>(left, right, result, count);
  }
}

}  // namespace

void VectorOperations::Sum(Vector &left, Vector &right, Vector &result, idx_t count) {
  ExecuteOperation<class Sum>(left, right, result, count);
}

void VectorOperations::Dot(Vector &left, Vector &right, Vector &result, idx_t count) {
  ExecuteOperation<class Dot>(left, right, result, count);
}

void VectorOperations::Division(Vector &left, Vector &right, Vector &result, idx_t count) {
  ExecuteOperation<class Division>(left, right, result, count);
}

void VectorOperations::Difference(Vector &left, Vector &right, Vector &result, idx_t count) {
  ExecuteOperation<class Difference>(left, right, result, count);
}

void VectorOperations::Modulo(Vector &left, Vector &right, Vector &result, idx_t count) {
  ExecuteOperation<class Modulo>(left, right, result, count);
}

void VectorOperations::LAnd(Vector &left, Vector &right, Vector &result, idx_t count) {
  ExecuteOperation<And>(left, right, result, count);
}

void VectorOperations::Negate(Vector &input, Vector &result, idx_t count) {
  switch (input.GetType()) {
    case PhysicalType::TINYINT:
      TemplatedExecuteNegateSwitchResult<int8_t>(input, result, count);
      break;
    case PhysicalType::SMALLINT:
      TemplatedExecuteNegateSwitchResult<int16_t>(input, result, count);
      break;
    case PhysicalType::INTEGER:
      TemplatedExecuteNegateSwitchResult<int32_t>(input, result, count);
      break;
    case PhysicalType::UTINYINT:
      TemplatedExecuteNegateSwitchResult<uint8_t>(input, result, count);
      break;
    case PhysicalType::USMALLINT:
      TemplatedExecuteNegateSwitchResult<uint16_t>(input, result, count);
      break;
    case PhysicalType::UINTEGER:
      TemplatedExecuteNegateSwitchResult<uint32_t>(input, result, count);
      break;
    case PhysicalType::UBIGINT:
      TemplatedExecuteNegateSwitchResult<uint64_t>(input, result, count);
      break;
    case PhysicalType::BIGINT:
      TemplatedExecuteNegateSwitchResult<int64_t>(input, result, count);
      break;
    case PhysicalType::FLOAT:
      TemplatedExecuteNegateSwitchResult<float>(input, result, count);
      break;
    case PhysicalType::DOUBLE:
      TemplatedExecuteNegateSwitchResult<double>(input, result, count);
      break;
    default:
      throw NotImplementedException(
          fmt::format("negate: unsupported input type {}", LogicalType::NameOf(input.GetType())));
  }
}

}  // namespace bumblebee
