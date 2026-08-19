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
#include <optional>
#include <vector>

#include "catalog/column.h"
#include "execution/expressions/arithmetic_expression.h"
#include "execution/expressions/cast_expression.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/in_expression.h"
#include "execution/expressions/is_null_expression.h"
#include "execution/expressions/like_expression.h"
#include "execution/expressions/logic_expression.h"
#include "gtest/gtest.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

const LogicalType kInt(LogicalTypeId::INTEGER);

static auto Col(uint32_t idx) -> AbstractExpressionRef {
  return std::make_shared<ColumnValueExpression>(0, idx, Column::Make("c", kInt));
}
static auto Const(int v) -> AbstractExpressionRef { return std::make_shared<ConstantValueExpression>(Value(v)); }

/** @brief A 2-column INT chunk: col0 = a, col1 = b. */
static auto MakeChunk(const std::vector<int> &a, const std::vector<int> &b) -> DataChunk {
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{kInt, kInt});
  for (idx_t i = 0; i < a.size(); i++) {
    chunk.SetValue(0, i, Value(a[i]));
    chunk.SetValue(1, i, Value(b[i]));
  }
  chunk.SetCardinality(a.size());
  return chunk;
}

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

// A 1-column STRING chunk from literals.
auto MakeStringChunk(const std::vector<std::string> &vals) -> DataChunk {
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{LogicalType(LogicalTypeId::STRING)});
  for (idx_t i = 0; i < vals.size(); i++) {
    chunk.SetValue(0, i, Value(vals[i]));
  }
  chunk.SetCardinality(vals.size());
  return chunk;
}

auto StrCol() -> AbstractExpressionRef {
  return std::make_shared<ColumnValueExpression>(0, 0, Column::Make("s", LogicalType(LogicalTypeId::STRING)));
}
auto Pattern(const std::string &p) -> AbstractExpressionRef {
  return std::make_shared<ConstantValueExpression>(Value(p));
}

TEST(ExpressionExecutorTest, LikeSelectsWildcardMatches) {
  auto chunk = MakeStringChunk({"google.com", "www.google.co", "bing", "MED BOX"});
  auto pred = std::make_shared<LikeExpression>(StrCol(), Pattern("%google%"), /*negated=*/false);

  ExpressionExecutor exec(*pred);
  SelectionVector sel(chunk.GetSize());
  idx_t n = exec.Select(chunk, sel);

  ASSERT_EQ(n, 2);
  EXPECT_EQ(sel.GetIndex(0), 0);
  EXPECT_EQ(sel.GetIndex(1), 1);
}

TEST(ExpressionExecutorTest, LikeUnderscoreAndAnchorsAndNotLike) {
  auto chunk = MakeStringChunk({"cat", "coat", "MED BOX"});
  // `c_t` = c + exactly one char + t -> "cat" only ("coat" is one char too long).
  auto underscore = std::make_shared<LikeExpression>(StrCol(), Pattern("c_t"), false);
  ExpressionExecutor e1(*underscore);
  SelectionVector s1(chunk.GetSize());
  ASSERT_EQ(e1.Select(chunk, s1), 1);
  EXPECT_EQ(s1.GetIndex(0), 0);  // "cat"

  // Suffix anchor.
  auto anchor = std::make_shared<LikeExpression>(StrCol(), Pattern("%BOX"), false);
  ExpressionExecutor e2(*anchor);
  SelectionVector s2(chunk.GetSize());
  ASSERT_EQ(e2.Select(chunk, s2), 1);
  EXPECT_EQ(s2.GetIndex(0), 2);  // "MED BOX"

  // NOT LIKE inverts: everything not starting with 'c'.
  auto not_like = std::make_shared<LikeExpression>(StrCol(), Pattern("c%"), /*negated=*/true);
  ExpressionExecutor e3(*not_like);
  SelectionVector s3(chunk.GetSize());
  ASSERT_EQ(e3.Select(chunk, s3), 1);
  EXPECT_EQ(s3.GetIndex(0), 2);  // "MED BOX"
}

// A literal '%' in the DATA aligned with the pattern's '%' must not be consumed as a literal
// (regression for the wildcard/literal precedence bug that broke LIKE over %-escaped URLs).
TEST(ExpressionExecutorTest, LikePercentInDataMatchesWildcard) {
  auto chunk = MakeStringChunk({"http%26rleurl", "httpXXrleurl", "ab"});
  // "____%" = at least 4 chars; all three of these differ only in length -> first two match.
  auto len4 = std::make_shared<LikeExpression>(StrCol(), Pattern("____%"), false);
  ExpressionExecutor e1(*len4);
  SelectionVector s1(chunk.GetSize());
  ASSERT_EQ(e1.Select(chunk, s1), 2);
  EXPECT_EQ(s1.GetIndex(0), 0);  // "http%26rleurl" (% at offset 4)
  EXPECT_EQ(s1.GetIndex(1), 1);  // "httpXXrleurl"

  // A wildcard right after a literal that IS '%' in the data.
  auto contains = std::make_shared<LikeExpression>(StrCol(), Pattern("http%rleurl"), false);
  ExpressionExecutor e2(*contains);
  SelectionVector s2(chunk.GetSize());
  ASSERT_EQ(e2.Select(chunk, s2), 2);  // both http... rows: "http" + <anything> + "rleurl"
}

TEST(ExpressionExecutorTest, InSelectsListMembers) {
  auto chunk = MakeChunk({1, 2, 3, 4, 5}, {0, 0, 0, 0, 0});
  auto in = std::make_shared<InExpression>(std::vector<AbstractExpressionRef>{Col(0), Const(1), Const(3)},
                                           /*negated=*/false);
  ExpressionExecutor e1(*in);
  SelectionVector s1(chunk.GetSize());
  ASSERT_EQ(e1.Select(chunk, s1), 2);
  EXPECT_EQ(s1.GetIndex(0), 0);  // value 1
  EXPECT_EQ(s1.GetIndex(1), 2);  // value 3

  // With no NULLs anywhere, NOT IN selects the exact complement.
  auto not_in = std::make_shared<InExpression>(std::vector<AbstractExpressionRef>{Col(0), Const(1), Const(3)},
                                               /*negated=*/true);
  ExpressionExecutor e2(*not_in);
  SelectionVector s2(chunk.GetSize());
  ASSERT_EQ(e2.Select(chunk, s2), 3);
  EXPECT_EQ(s2.GetIndex(0), 1);
  EXPECT_EQ(s2.GetIndex(1), 3);
  EXPECT_EQ(s2.GetIndex(2), 4);
}

// A 1-column INT chunk where nullopt becomes SQL NULL.
auto MakeNullableIntChunk(const std::vector<std::optional<int>> &vals) -> DataChunk {
  DataChunk chunk;
  chunk.Initialize(std::vector<LogicalType>{kInt});
  for (idx_t i = 0; i < vals.size(); i++) {
    chunk.SetValue(0, i, vals[i].has_value() ? Value(*vals[i]) : Value::Null(kInt));
  }
  chunk.SetCardinality(vals.size());
  return chunk;
}

// SQL three-valued logic folded to 0/1: a non-match that involved a NULL is NULL in SQL, which
// must select nothing for BOTH polarities — the reason NOT IN cannot be an outer NOT over IN.
TEST(ExpressionExecutorTest, InNullSemantics) {
  auto chunk = MakeNullableIntChunk({1, 2, std::nullopt});
  auto null_const = std::make_shared<ConstantValueExpression>(Value::Null(kInt));

  // v IN (1, NULL): only the match survives; NULL-poisoned non-matches select nothing.
  auto in = std::make_shared<InExpression>(std::vector<AbstractExpressionRef>{Col(0), Const(1), null_const},
                                           /*negated=*/false);
  ExpressionExecutor e1(*in);
  SelectionVector s1(chunk.GetSize());
  ASSERT_EQ(e1.Select(chunk, s1), 1);
  EXPECT_EQ(s1.GetIndex(0), 0);

  // v NOT IN (1, NULL): the NULL in the list poisons every non-match -> empty (the classic).
  auto not_in_null = std::make_shared<InExpression>(std::vector<AbstractExpressionRef>{Col(0), Const(1), null_const},
                                                    /*negated=*/true);
  ExpressionExecutor e2(*not_in_null);
  SelectionVector s2(chunk.GetSize());
  ASSERT_EQ(e2.Select(chunk, s2), 0);

  // v NOT IN (1): a NULL tested value never qualifies; the clean non-match does.
  auto not_in = std::make_shared<InExpression>(std::vector<AbstractExpressionRef>{Col(0), Const(1)},
                                               /*negated=*/true);
  ExpressionExecutor e3(*not_in);
  SelectionVector s3(chunk.GetSize());
  ASSERT_EQ(e3.Select(chunk, s3), 1);
  EXPECT_EQ(s3.GetIndex(0), 1);  // value 2
}

// An all-constant integer list is sorted once and answered by binary search. The prepared list is
// cached on the executor, so it has to survive being reused across chunks, and it has to agree with
// the general kernel on the awkward inputs: unsorted, duplicated and negative elements.
TEST(ExpressionExecutorTest, InPreparedListMatchesGeneralKernel) {
  std::vector<AbstractExpressionRef> children{Col(0)};
  for (const int v : {7, -3, 7, 0, 4, -3}) {  // unsorted, duplicated, negative
    children.push_back(Const(v));
  }
  auto in = std::make_shared<InExpression>(children, /*negated=*/false);
  ExpressionExecutor exec(*in);

  auto first = MakeChunk({-3, 1, 4, 7, 5}, {0, 0, 0, 0, 0});
  SelectionVector s1(first.GetSize());
  ASSERT_EQ(exec.Select(first, s1), 3);
  EXPECT_EQ(s1.GetIndex(0), 0);  // -3
  EXPECT_EQ(s1.GetIndex(1), 2);  // 4
  EXPECT_EQ(s1.GetIndex(2), 3);  // 7

  // The SAME executor over a second chunk reuses the prepared list.
  auto second = MakeChunk({0, 6, -4, 7, 2}, {0, 0, 0, 0, 0});
  SelectionVector s2(second.GetSize());
  ASSERT_EQ(exec.Select(second, s2), 2);
  EXPECT_EQ(s2.GetIndex(0), 0);  // 0
  EXPECT_EQ(s2.GetIndex(1), 3);  // 7
}

// A list element that is not a constant cannot be prepared: the general kernel must still run it.
TEST(ExpressionExecutorTest, InWithNonConstantElementFallsBack) {
  // Column 0 is the tested value, column 1 supplies a per-row list element.
  auto chunk = MakeChunk({1, 2, 3}, {9, 2, 9});
  auto in = std::make_shared<InExpression>(std::vector<AbstractExpressionRef>{Col(0), Const(1), Col(1)},
                                           /*negated=*/false);
  ExpressionExecutor exec(*in);
  SelectionVector sel(chunk.GetSize());
  ASSERT_EQ(exec.Select(chunk, sel), 2);
  EXPECT_EQ(sel.GetIndex(0), 0);  // 1 IN (1, 9)
  EXPECT_EQ(sel.GetIndex(1), 1);  // 2 IN (1, 2) — matches the row's own second column
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

TEST(ExpressionExecutorTest, IsNullReadsValidityNotValues) {
  // Rows 1 and 3 are NULL. IS NULL selects exactly those; IS NOT NULL the complement.
  auto chunk = MakeChunk({5, 0, 7, 0}, {0, 0, 0, 0});
  chunk.SetValue(0, 1, Value::Null(kInt));
  chunk.SetValue(0, 3, Value::Null(kInt));

  auto is_null = std::make_shared<IsNullExpression>(Col(0), /*negated=*/false);
  ExpressionExecutor exec(*is_null);
  SelectionVector sel(chunk.GetSize());
  ASSERT_EQ(exec.Select(chunk, sel), 2U);
  EXPECT_EQ(sel.GetIndex(0), 1U);
  EXPECT_EQ(sel.GetIndex(1), 3U);

  auto is_not_null = std::make_shared<IsNullExpression>(Col(0), /*negated=*/true);
  ExpressionExecutor exec_not(*is_not_null);
  SelectionVector not_sel(chunk.GetSize());
  ASSERT_EQ(exec_not.Select(chunk, not_sel), 2U);
  EXPECT_EQ(not_sel.GetIndex(0), 0U);
  EXPECT_EQ(not_sel.GetIndex(1), 2U);
}

TEST(ExpressionExecutorTest, CastFromUnknownIsNullBroadcast) {
  // The untyped NULL literal (UNKNOWN) casts to any type as a NULL of that type — this is what
  // lets `INSERT ... VALUES (NULL)` / `SET col = NULL` target VARCHAR/BIGINT/DOUBLE columns.
  auto chunk = MakeChunk({1, 2, 3}, {0, 0, 0});
  auto null_const = std::make_shared<ConstantValueExpression>(Value::Null());
  ASSERT_EQ(null_const->GetReturnType().GetType().GetTypeId(), LogicalTypeId::UNKNOWN);

  for (const auto target : {LogicalTypeId::STRING, LogicalTypeId::BIGINT, LogicalTypeId::DOUBLE}) {
    auto cast = std::make_shared<CastExpression>(null_const, LogicalType(target));
    ExpressionExecutor exec(*cast);
    Vector out{LogicalType(target)};
    exec.ExecuteExpression(chunk, out);
    for (idx_t i = 0; i < chunk.GetSize(); i++) {
      EXPECT_TRUE(out.GetValue(i).IsNull());
    }
  }
}

}  // namespace bumblebee
