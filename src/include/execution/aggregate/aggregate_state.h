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

#include "execution/plans/aggregation_plan.h"
#include "type/logical_type.h"
#include "type/value.h"

namespace bumblebee {

/**
 * @brief A running accumulator for one aggregate over one group — the B7 aggregate state, value-at-a-time.
 *
 * Numeric only for now (accumulates through `long double`, which carries the 64-bit integer range); a
 * string MIN/MAX is a future extension. NULL inputs are ignored except by COUNT(*), matching SQL.
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
    const auto d = v.GetAs<long double>();
    switch (type_) {
      case AggregationType::SumAggregate:
        sum_ += d;
        break;
      case AggregationType::MinAggregate:
        acc_ = has_acc_ ? (d < acc_ ? d : acc_) : d;
        has_acc_ = true;
        break;
      case AggregationType::MaxAggregate:
        acc_ = has_acc_ ? (d > acc_ ? d : acc_) : d;
        has_acc_ = true;
        break;
      default:
        break;  // Count: the count_++ above is all it needs
    }
  }

  /** @brief Merge another accumulator of the same aggregate into this one (for Combine). */
  void Merge(const AggregateAccumulator &o) {
    count_ += o.count_;
    sum_ += o.sum_;
    if (o.has_acc_) {
      if (!has_acc_) {
        acc_ = o.acc_;
      } else if (type_ == AggregationType::MinAggregate) {
        acc_ = o.acc_ < acc_ ? o.acc_ : acc_;
      } else if (type_ == AggregationType::MaxAggregate) {
        acc_ = o.acc_ > acc_ ? o.acc_ : acc_;
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
      case AggregationType::MinAggregate:
      case AggregationType::MaxAggregate:
        return has_acc_ ? Value(static_cast<double>(acc_)).CastAs(out_type) : Value::Null(out_type);
    }
    return Value::Null(out_type);
  }

 private:
  AggregationType type_;
  int64_t count_{0};
  long double sum_{0};
  long double acc_{0};
  bool has_acc_{false};
};

}  // namespace bumblebee
