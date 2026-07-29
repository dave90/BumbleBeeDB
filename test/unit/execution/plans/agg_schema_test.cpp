//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// agg_schema_test.cpp
//
// Identification: test/unit/execution/plans/agg_schema_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/plans/aggregation_plan.h"

#include <memory>
#include <vector>

#include "catalog/column.h"
#include "execution/expressions/column_value_expression.h"
#include "gtest/gtest.h"
#include "type/logical_type.h"

namespace bumblebee {

namespace {

/** @brief A single-aggregate schema over an argument of the given type. */
auto AggOutputType(const LogicalType &arg_type, AggregationType agg) -> LogicalTypeId {
  std::vector<AbstractExpressionRef> aggregates;
  aggregates.push_back(
      std::make_shared<ColumnValueExpression>(0, 0, Column::Make("a", arg_type)));
  auto schema = AggregationPlanNode::InferAggSchema({}, aggregates, {agg});
  return schema.GetColumn(0).GetType().GetTypeId();
}

}  // namespace

// SUM widens every integer width to BIGINT: summing many INT32s overflows 32 bits, so the
// output type must be 64-bit (regression for the benchmark's SUM(int32) -> INT32_MAX bug).
TEST(AggSchemaTest, SumWidensIntegersToBigint) {
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::TINYINT), AggregationType::SumAggregate),
            LogicalTypeId::BIGINT);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::SMALLINT), AggregationType::SumAggregate),
            LogicalTypeId::BIGINT);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::INTEGER), AggregationType::SumAggregate),
            LogicalTypeId::BIGINT);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::BIGINT), AggregationType::SumAggregate),
            LogicalTypeId::BIGINT);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::UINTEGER), AggregationType::SumAggregate),
            LogicalTypeId::BIGINT);
}

// SUM of a floating or decimal column keeps that family's type (no spurious widening).
TEST(AggSchemaTest, SumKeepsFloatingAndDecimal) {
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::FLOAT), AggregationType::SumAggregate),
            LogicalTypeId::FLOAT);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::DOUBLE), AggregationType::SumAggregate),
            LogicalTypeId::DOUBLE);
  EXPECT_EQ(AggOutputType(LogicalType::Decimal(18, 2), AggregationType::SumAggregate),
            LogicalTypeId::DECIMAL);
}

// AVG is always DOUBLE, whatever it averages.
TEST(AggSchemaTest, AvgIsAlwaysDouble) {
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::INTEGER), AggregationType::AvgAggregate),
            LogicalTypeId::DOUBLE);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::BIGINT), AggregationType::AvgAggregate),
            LogicalTypeId::DOUBLE);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::DOUBLE), AggregationType::AvgAggregate),
            LogicalTypeId::DOUBLE);
  EXPECT_EQ(AggOutputType(LogicalType::Decimal(18, 2), AggregationType::AvgAggregate),
            LogicalTypeId::DOUBLE);
}

// MIN/MAX are unchanged by the fix: they still report the input type.
TEST(AggSchemaTest, MinMaxPreserveInputType) {
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::INTEGER), AggregationType::MinAggregate),
            LogicalTypeId::INTEGER);
  EXPECT_EQ(AggOutputType(LogicalType(LogicalTypeId::INTEGER), AggregationType::MaxAggregate),
            LogicalTypeId::INTEGER);
}

}  // namespace bumblebee
