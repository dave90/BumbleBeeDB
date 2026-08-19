//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// expression_test.cpp
//
// Identification: test/unit/execution/expression_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "execution/expressions/arithmetic_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/expressions/string_expression.h"
#include "gtest/gtest.h"

namespace bumblebee {

static auto IntColumn(uint32_t tuple_idx, uint32_t col_idx) -> AbstractExpressionRef {
  return std::make_shared<ColumnValueExpression>(tuple_idx, col_idx, Column{"c", LogicalType(LogicalTypeId::INTEGER)});
}

static auto IntConst(int32_t v) -> AbstractExpressionRef { return std::make_shared<ConstantValueExpression>(Value{v}); }

TEST(ExpressionTest, ColumnValueToString) {
  EXPECT_EQ(IntColumn(0, 1)->ToString(), "#0.1");
  EXPECT_EQ(IntColumn(1, 0)->ToString(), "#1.0");
}

TEST(ExpressionTest, ConstantToString) {
  EXPECT_EQ(IntConst(42)->ToString(), "42");
  EXPECT_EQ(ConstantValueExpression(Value{"abc"}).ToString(), "'abc'");
  EXPECT_EQ(ConstantValueExpression(Value{true}).ToString(), "true");
}

TEST(ExpressionTest, ConstantReturnTypeFollowsTheValue) {
  EXPECT_EQ(ConstantValueExpression(Value{true}).GetReturnType().GetType(), LogicalType(LogicalTypeId::BOOLEAN));
  // A VARCHAR constant is variable-length; Column::Make must not trip the
  // fixed-width assertion.
  EXPECT_EQ(ConstantValueExpression(Value{"abc"}).GetReturnType().GetType(), LogicalType(LogicalTypeId::STRING));
}

TEST(ExpressionTest, ComparisonToString) {
  ComparisonExpression eq{IntColumn(0, 0), IntConst(1), ComparisonType::Equal};
  EXPECT_EQ(eq.ToString(), "(#0.0=1)");
  EXPECT_EQ(eq.GetReturnType().GetType(), LogicalType(LogicalTypeId::BOOLEAN));

  ComparisonExpression gt{IntColumn(0, 2), IntConst(10), ComparisonType::GreaterThan};
  EXPECT_EQ(gt.ToString(), "(#0.2>10)");
}

TEST(ExpressionTest, ArithmeticToString) {
  ArithmeticExpression plus{IntColumn(0, 0), IntConst(1), ArithmeticType::Plus};
  EXPECT_EQ(plus.ToString(), "(#0.0+1)");
  EXPECT_EQ(plus.GetReturnType().GetType(), LogicalType(LogicalTypeId::INTEGER));
}

TEST(ExpressionTest, ArithmeticWidensToTheCommonType) {
  auto big = std::make_shared<ColumnValueExpression>(0, 0, Column{"c", LogicalType(LogicalTypeId::BIGINT)});
  ArithmeticExpression plus{IntColumn(0, 1), big, ArithmeticType::Plus};
  EXPECT_EQ(plus.GetReturnType().GetType(), LogicalType(LogicalTypeId::BIGINT));
}

TEST(ExpressionTest, LogicToString) {
  auto left = std::make_shared<ComparisonExpression>(IntColumn(0, 0), IntConst(1), ComparisonType::Equal);
  auto right = std::make_shared<ComparisonExpression>(IntColumn(0, 1), IntConst(2), ComparisonType::Equal);
  LogicExpression conj{left, right, LogicType::And};
  EXPECT_EQ(conj.ToString(), "((#0.0=1)and(#0.1=2))");
}

TEST(ExpressionTest, HasOrPredicate) {
  auto a = std::make_shared<ComparisonExpression>(IntColumn(0, 0), IntConst(1), ComparisonType::Equal);
  auto b = std::make_shared<ComparisonExpression>(IntColumn(0, 1), IntConst(2), ComparisonType::Equal);
  auto c = std::make_shared<ComparisonExpression>(IntColumn(0, 2), IntConst(3), ComparisonType::Equal);

  AbstractExpressionRef conj = std::make_shared<LogicExpression>(a, b, LogicType::And);
  EXPECT_FALSE(LogicExpression::HasOrPredicate(conj));
  EXPECT_FALSE(LogicExpression::HasOrPredicate(a));

  AbstractExpressionRef disj = std::make_shared<LogicExpression>(a, b, LogicType::Or);
  EXPECT_TRUE(LogicExpression::HasOrPredicate(disj));

  // An OR buried under an AND must still be found — this is what stops the
  // hash-join rule from firing on a disjunctive predicate.
  AbstractExpressionRef nested = std::make_shared<LogicExpression>(conj, disj, LogicType::And);
  EXPECT_TRUE(LogicExpression::HasOrPredicate(nested));
  AbstractExpressionRef nested2 = std::make_shared<LogicExpression>(disj, c, LogicType::And);
  EXPECT_TRUE(LogicExpression::HasOrPredicate(nested2));
}

TEST(ExpressionTest, StringExpression) {
  auto str = std::make_shared<ColumnValueExpression>(0, 0, Column{"s", LogicalType(LogicalTypeId::STRING), 32});
  StringExpression lower{str, StringExpressionType::Lower};
  EXPECT_EQ(lower.ToString(), "lower(#0.0)");
  EXPECT_EQ(lower.GetReturnType().GetType(), LogicalType(LogicalTypeId::STRING));

  EXPECT_THROW(StringExpression(IntColumn(0, 0), StringExpressionType::Upper), NotImplementedException);
}

TEST(ExpressionTest, CloneWithChildren) {
  ComparisonExpression eq{IntColumn(0, 0), IntConst(1), ComparisonType::Equal};
  auto clone = eq.CloneWithChildren({IntColumn(1, 5), IntConst(9)});
  EXPECT_EQ(clone->ToString(), "(#1.5=9)");
  // The original is untouched.
  EXPECT_EQ(eq.ToString(), "(#0.0=1)");
}

}  // namespace bumblebee
