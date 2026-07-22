//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// expression_executor_test.cpp
//
// Identification: test/unit/execution/expression_executor_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/expression_executor.h"

#include <memory>
#include <vector>

#include "catalog/column.h"
#include "execution/expressions/arithmetic_expression.h"
#include "execution/expressions/cast_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "gtest/gtest.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);

auto Col(uint32_t idx) -> AbstractExpressionRef {
  return std::make_shared<ColumnValueExpression>(0, idx, Column::Make("c", kInt));
}
auto Const(int v) -> AbstractExpressionRef { return std::make_shared<ConstantValueExpression>(Value(v)); }

/** @brief A 2-column INT chunk: col0 = a, col1 = b. */
auto MakeChunk(const std::vector<int> &a, const std::vector<int> &b) -> DataChunk {
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{kInt, kInt});
  for (idx_t i = 0; i < a.size(); i++) {
    chunk.SetValue(0, i, Value(a[i]));
    chunk.SetValue(1, i, Value(b[i]));
  }
  chunk.SetCardinality(a.size());
  return chunk;
}

}  // namespace

TEST(ExpressionExecutorTest, SelectGreaterThanConstant) {
  auto chunk = MakeChunk({1, 6, 3, 10, 5}, {0, 0, 0, 0, 0});
  auto pred = std::make_shared<ComparisonExpression>(Col(0), Const(5), ComparisonType::GreaterThan);

  ExpressionExecutor exec(*pred);
  SelectionVector sel(chunk.GetSize());
  idx_t n = exec.Select(chunk, sel);

  ASSERT_EQ(n, 2);
  EXPECT_EQ(sel.GetIndex(0), 1);  // value 6
  EXPECT_EQ(sel.GetIndex(1), 3);  // value 10
}

TEST(ExpressionExecutorTest, SelectAndOfTwoComparisons) {
  auto chunk = MakeChunk({1, 6, 3, 10, 5}, {9, 9, 1, 9, 9});
  // (a > 4) AND (b > 5)  -> a>4: {1,3,4}; b>5: {0,1,3,4}; AND: idx1 (6,9), idx3 (10,9), idx4 (5,9)
  auto left = std::make_shared<ComparisonExpression>(Col(0), Const(4), ComparisonType::GreaterThan);
  auto right = std::make_shared<ComparisonExpression>(Col(1), Const(5), ComparisonType::GreaterThan);
  auto pred = std::make_shared<LogicExpression>(left, right, LogicType::And);

  ExpressionExecutor exec(*pred);
  SelectionVector sel(chunk.GetSize());
  idx_t n = exec.Select(chunk, sel);

  ASSERT_EQ(n, 3);
  EXPECT_EQ(sel.GetIndex(0), 1);
  EXPECT_EQ(sel.GetIndex(1), 3);
  EXPECT_EQ(sel.GetIndex(2), 4);
}

TEST(ExpressionExecutorTest, ArithmeticAddColumns) {
  auto chunk = MakeChunk({1, 2, 3}, {10, 20, 30});
  auto sum = std::make_shared<ArithmeticExpression>(Col(0), Col(1), ArithmeticType::Plus);

  ExpressionExecutor exec(*sum);
  Vector out(kInt);
  exec.ExecuteExpression(chunk, out);

  EXPECT_EQ(out.GetValue(0).GetAs<int>(), 11);
  EXPECT_EQ(out.GetValue(1).GetAs<int>(), 22);
  EXPECT_EQ(out.GetValue(2).GetAs<int>(), 33);
}

TEST(ExpressionExecutorTest, ProjectionReordersColumnsZeroCopy) {
  auto chunk = MakeChunk({1, 2, 3}, {10, 20, 30});
  // Project [col1, col0] -> swaps the two columns.
  std::vector<const AbstractExpression *> exprs;
  auto c1 = Col(1);
  auto c0 = Col(0);
  exprs.push_back(c1.get());
  exprs.push_back(c0.get());

  ExpressionExecutor exec(exprs);
  DataChunk out;
  out.InitializeEmpty(std::vector<LogicalType>{kInt, kInt});
  exec.Execute(chunk, out);

  ASSERT_EQ(out.GetSize(), 3);
  EXPECT_EQ(out.GetValue(0, 0).GetAs<int>(), 10);
  EXPECT_EQ(out.GetValue(1, 0).GetAs<int>(), 1);
  EXPECT_EQ(out.GetValue(0, 2).GetAs<int>(), 30);
  EXPECT_EQ(out.GetValue(1, 2).GetAs<int>(), 3);
}

TEST(ExpressionExecutorTest, CastIntColumnToBigint) {
  const LogicalType kBig(LogicalTypeId::BIGINT);
  auto chunk = MakeChunk({1, 2000000000, -7}, {0, 0, 0});
  // cast(col0 AS BIGINT): the values must survive the widening (and the output must be 64-bit).
  auto cast = std::make_shared<CastExpression>(Col(0), kBig);

  ExpressionExecutor exec(*cast);
  Vector out(kBig);
  exec.ExecuteExpression(chunk, out);

  EXPECT_EQ(out.GetType(), PhysicalType::BIGINT);
  EXPECT_EQ(out.GetValue(0).GetAs<int64_t>(), 1);
  EXPECT_EQ(out.GetValue(1).GetAs<int64_t>(), 2000000000);
  EXPECT_EQ(out.GetValue(2).GetAs<int64_t>(), -7);
}

TEST(ExpressionExecutorTest, CastToSameTypeIsIdentity) {
  auto chunk = MakeChunk({5, 6, 7}, {0, 0, 0});
  auto cast = std::make_shared<CastExpression>(Col(0), kInt);  // INT -> INT: no-op

  ExpressionExecutor exec(*cast);
  Vector out(kInt);
  exec.ExecuteExpression(chunk, out);

  EXPECT_EQ(out.GetValue(0).GetAs<int>(), 5);
  EXPECT_EQ(out.GetValue(2).GetAs<int>(), 7);
}

}  // namespace bumblebee
