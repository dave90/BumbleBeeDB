//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// aggregate_state.h
//
// Identification: src/include/execution/aggregate/aggregate_state.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <type_traits>

#include "execution/aggregate/aggregate_semantics.h"
#include "execution/plans/aggregation_plan.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/vector.h"

namespace bumblebee {

/**
 * @brief A running accumulator for one aggregate over one group — the B7 aggregate state.
 *
 * Numerics accumulate through `long double` (which carries the 64-bit integer range); MIN/MAX also
 * accept a string argument, comparing lexicographically (SQL VARCHAR ordering) and keeping an owned
 * copy. NULL inputs are ignored except by COUNT(*), matching SQL.
 *
 * Two update paths: `Update(Value)` folds one row (used where rows scatter across group states), and
 * `UpdateVector` folds a whole vector in one columnar pass — a constant collapses to O(1), a flat
 * vector runs a tight per-physical-type loop over sequential data (no per-row Value boxing, no
 * selection indirection), which the compiler can unroll and auto-vectorize.
 */
class AggregateAccumulator {
 public:
  explicit AggregateAccumulator(AggregationType type) : type_(type) {}

  /** @brief Fold one input value (the aggregate argument; ignored for COUNT(*)) into the state. */
  void Update(const Value &v) {
    if (type_ == AggregationType::CountStarAggregate) {
      count_++;
      return;
    }
    if (v.IsNull()) {
      return;  // COUNT / SUM / MIN / MAX all skip NULLs
    }
    count_++;
    if (IsStringMinMax(v)) {
      FoldStr(v.GetString());
      return;
    }
    const auto d = v.GetAs<long double>();
    switch (type_) {
      case AggregationType::AvgAggregate:  // AVG accumulates like SUM; it divides by count only at finalize.
      case AggregationType::SumAggregate:
        sum_ += d;
        break;
      case AggregationType::MinAggregate:
        FoldMin(d);
        break;
      case AggregationType::MaxAggregate:
        FoldMax(d);
        break;
      default:
        break;  // Count: the count_++ above is all it needs
    }
  }

  /**
   * @brief Fold a whole vector of aggregate arguments into the state in one columnar pass.
   *
   * COUNT(*) never reads the data at all; a CONSTANT vector is folded in O(1); a FLAT vector takes
   * the typed tight-loop path below; any other encoding (dictionary, sequence) is normalified first
   * so the hot loop always runs over contiguous data.
   */
  void UpdateVector(Vector &v, idx_t count) {
    if (count == 0) {
      return;
    }
    if (type_ == AggregationType::CountStarAggregate) {
      count_ += static_cast<int64_t>(count);
      return;
    }
    if (v.GetVectorType() == VectorType::CONSTANT_VECTOR) {
      const auto val = v.GetValue(0);
      if (val.IsNull()) {
        return;  // a NULL constant contributes nothing (COUNT(*) was handled above)
      }
      count_ += static_cast<int64_t>(count);
      if (IsStringMinMax(val)) {
        FoldStr(val.GetString());  // every row is the same string; one fold captures the min/max
        return;
      }
      const auto d = val.GetAs<long double>();
      if (IsSumLike(type_)) {
        sum_ += d * static_cast<long double>(count);
      } else if (type_ == AggregationType::MinAggregate) {
        FoldMin(d);
      } else if (type_ == AggregationType::MaxAggregate) {
        FoldMax(d);
      }
      return;
    }
    if (v.GetVectorType() != VectorType::FLAT_VECTOR) {
      v.Normalify(count);
    }
    switch (v.GetLogicalType().GetPhysicalType()) {
      case PhysicalType::TINYINT:
        return UpdateFlat<int8_t>(FlatVector::GetData<int8_t>(v), v.Validity(), count);
      case PhysicalType::SMALLINT:
        return UpdateFlat<int16_t>(FlatVector::GetData<int16_t>(v), v.Validity(), count);
      case PhysicalType::INTEGER:
        return UpdateFlat<int32_t>(FlatVector::GetData<int32_t>(v), v.Validity(), count);
      case PhysicalType::BIGINT:
        return UpdateFlat<int64_t>(FlatVector::GetData<int64_t>(v), v.Validity(), count);
      case PhysicalType::UTINYINT:
        return UpdateFlat<uint8_t>(FlatVector::GetData<uint8_t>(v), v.Validity(), count);
      case PhysicalType::USMALLINT:
        return UpdateFlat<uint16_t>(FlatVector::GetData<uint16_t>(v), v.Validity(), count);
      case PhysicalType::UINTEGER:
        return UpdateFlat<uint32_t>(FlatVector::GetData<uint32_t>(v), v.Validity(), count);
      case PhysicalType::UBIGINT:
        return UpdateFlat<uint64_t>(FlatVector::GetData<uint64_t>(v), v.Validity(), count);
      case PhysicalType::FLOAT:
        return UpdateFlat<float>(FlatVector::GetData<float>(v), v.Validity(), count);
      case PhysicalType::DOUBLE:
        return UpdateFlat<double>(FlatVector::GetData<double>(v), v.Validity(), count);
      default:
        // Non-numeric physical type: fall back to the row-at-a-time path (same NULL semantics).
        for (idx_t i = 0; i < count; i++) {
          Update(v.GetValue(i));
        }
    }
  }

  /** @brief Merge another accumulator of the same aggregate into this one (for Combine). */
  void Merge(const AggregateAccumulator &o) {
    count_ += o.count_;
    sum_ += o.sum_;
    if (o.has_acc_) {
      if (o.is_string_) {
        FoldStr(o.str_acc_);  // string MIN/MAX: fold the other's extreme in (sets is_string_/has_acc_)
      } else if (!has_acc_) {
        acc_ = o.acc_;  // nothing here yet: adopt the other side's extreme
      } else if (IsExtremeAgg(type_)) {
        acc_ = FoldExtreme(type_, acc_, o.acc_);
      }
      has_acc_ = true;
    }
  }

  /** @brief The final aggregate value, cast to the output column type. */
  auto Finalize(const LogicalType &out_type) const -> Value {
    switch (type_) {
      case AggregationType::CountStarAggregate:
      case AggregationType::CountAggregate:
        return Value(static_cast<int64_t>(count_)).CastAs(out_type);
      case AggregationType::SumAggregate:
        return count_ == 0 ? Value::Null(out_type) : Value(static_cast<double>(sum_)).CastAs(out_type);
      case AggregationType::AvgAggregate:
        // AVG = value-sum / count, as DOUBLE; a zero-count (all-NULL) input is NULL.
        return count_ == 0 ? Value::Null(out_type)
                           : Value(static_cast<double>(sum_ / static_cast<long double>(count_))).CastAs(out_type);
      case AggregationType::MinAggregate:
      case AggregationType::MaxAggregate:
        if (!has_acc_) {
          return Value::Null(out_type);
        }
        return is_string_ ? Value(str_acc_).CastAs(out_type) : Value(static_cast<double>(acc_)).CastAs(out_type);
    }
    return Value::Null(out_type);
  }

 private:
  /** @return True when this is MIN/MAX and the argument is a string (the lexicographic path). */
  auto IsStringMinMax(const Value &v) const -> bool {
    return IsExtremeAgg(type_) && v.GetPhysicalType() == PhysicalType::STRING;
  }

  /** @brief Fold one string into the running MIN/MAX, keeping an owned copy of the extreme. */
  void FoldStr(const std::string &s) {
    if (!has_acc_) {
      str_acc_ = s;
      is_string_ = true;
      has_acc_ = true;
      return;
    }
    if (TakesExtreme(type_, str_acc_, s)) {
      str_acc_ = s;
    }
  }

  /** @brief Fold one value into the scalar running extreme; the first value seeds it. */
  void FoldMin(long double d) {
    acc_ = has_acc_ ? FoldExtreme<true>(acc_, d) : d;
    has_acc_ = true;
  }
  void FoldMax(long double d) {
    acc_ = has_acc_ ? FoldExtreme<false>(acc_, d) : d;
    has_acc_ = true;
  }

  /** @brief Lane-reducible min/max for one element: NaN-dropping builtins for floats (they lower
   * to `fminnm`/`fmaxnm` lanes, which the strict `<`-select form would not), plain select for ints. */
  template <class T>
  static auto LaneMin(T a, T b) -> T {
    if constexpr (std::is_same_v<T, float>) {
      return __builtin_fminf(a, b);
    } else if constexpr (std::is_same_v<T, double>) {
      return __builtin_fmin(a, b);
    } else {
      return b < a ? b : a;
    }
  }
  template <class T>
  static auto LaneMax(T a, T b) -> T {
    if constexpr (std::is_same_v<T, float>) {
      return __builtin_fmaxf(a, b);
    } else if constexpr (std::is_same_v<T, double>) {
      return __builtin_fmax(a, b);
    } else {
      return b > a ? b : a;
    }
  }

  /**
   * @brief The columnar hot loop: fold `count` contiguous values of physical type `T`.
   *
   * The chunk accumulates in a plain register type — int64 for the integers, double for the
   * floats — and folds into the wide state ONCE at the end, so the inner loop is pure `acc += x` /
   * `min(acc, x)` over sequential memory: branch-free when the chunk has no NULLs, and exactly the
   * shape the auto-vectorizer turns into SIMD (verified with -Rpass=loop-vectorize: widening
   * integer sum lanes, smin/smax lanes, fadd/fminnm lanes). Two FP details make the float paths
   * eligible: the SUM loop opts into reassociation (strict IEEE ordering would force a serial
   * dependency chain — every vectorized engine reduces per-lane), and min/max go through
   * LaneMin/LaneMax. The NULL-carrying variant walks the validity mask per row but still reads
   * the data sequentially.
   */
  template <class T>
  void UpdateFlat(const T *data, const ValidityMask &validity, idx_t count) {
    using Acc = std::conditional_t<std::is_integral_v<T>, int64_t, double>;
    if (validity.AllValid()) {
      count_ += static_cast<int64_t>(count);
      switch (type_) {
        case AggregationType::AvgAggregate:  // AVG sums like SUM; the divide happens at finalize.
        case AggregationType::SumAggregate: {
          Acc local{};
          {
#if defined(__clang__)
#pragma clang fp reassociate(on)
#endif
            for (idx_t i = 0; i < count; i++) {
              local += static_cast<Acc>(data[i]);
            }
          }
          sum_ += static_cast<long double>(local);
          break;
        }
        case AggregationType::MinAggregate: {
          T m = data[0];
          for (idx_t i = 1; i < count; i++) {
            m = LaneMin(m, data[i]);
          }
          FoldMin(static_cast<long double>(m));
          break;
        }
        case AggregationType::MaxAggregate: {
          T m = data[0];
          for (idx_t i = 1; i < count; i++) {
            m = LaneMax(m, data[i]);
          }
          FoldMax(static_cast<long double>(m));
          break;
        }
        default:
          break;  // COUNT: the bump above is all it needs
      }
      return;
    }

    // NULLs present: skip invalid rows, count the valid ones (COUNT/SUM/MIN/MAX all count non-NULLs).
    idx_t valid = 0;
    switch (type_) {
      case AggregationType::AvgAggregate:  // AVG sums like SUM; the divide happens at finalize.
      case AggregationType::SumAggregate: {
        Acc local{};
        for (idx_t i = 0; i < count; i++) {
          if (validity.RowIsValid(i)) {
            local += static_cast<Acc>(data[i]);
            valid++;
          }
        }
        sum_ += static_cast<long double>(local);
        break;
      }
      case AggregationType::MinAggregate:
      case AggregationType::MaxAggregate: {
        bool seen = false;
        T m{};
        const bool want_min = type_ == AggregationType::MinAggregate;
        for (idx_t i = 0; i < count; i++) {
          if (!validity.RowIsValid(i)) {
            continue;
          }
          // `want_min` stays a hoisted local rather than a `type_` read: this loop is tuned to
          // vectorize, and a member load the compiler cannot prove non-aliasing with `data` would
          // stop it.
          m = !seen ? data[i] : (want_min ? FoldExtreme<true>(m, data[i]) : FoldExtreme<false>(m, data[i]));
          seen = true;
          valid++;
        }
        if (seen) {
          if (want_min) {
            FoldMin(static_cast<long double>(m));
          } else {
            FoldMax(static_cast<long double>(m));
          }
        }
        break;
      }
      default: {  // COUNT(x): just count the valid rows
        for (idx_t i = 0; i < count; i++) {
          valid += validity.RowIsValid(i) ? 1 : 0;
        }
        break;
      }
    }
    count_ += static_cast<int64_t>(valid);
  }

  AggregationType type_;
  int64_t count_{0};
  long double sum_{0};
  long double acc_{0};
  bool has_acc_{false};
  /** MIN/MAX over a string keeps its extreme here (owned); `is_string_` selects it over `acc_`. */
  bool is_string_{false};
  std::string str_acc_;
};

}  // namespace bumblebee
