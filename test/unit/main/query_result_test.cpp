//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// query_result_test.cpp
//
// Identification: test/unit/main/query_result_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/query_result.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "bumblebee_instance.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "gtest/gtest.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

static auto OneChunk(const std::vector<LogicalType> &types, const std::vector<std::vector<Value>> &rows)
    -> std::unique_ptr<DataChunk> {
  auto chunk = std::make_unique<DataChunk>();
  chunk->Initialize(types);
  for (idx_t row_idx = 0; row_idx < rows.size(); row_idx++) {
    for (idx_t column_idx = 0; column_idx < rows[row_idx].size(); column_idx++) {
      chunk->SetValue(column_idx, row_idx, rows[row_idx][column_idx]);
    }
  }
  chunk->SetCardinality(rows.size());
  return chunk;
}

TEST(QueryResultTest, OwnsFixedWidthStringsNullsListsAndArrays) {
  const auto integer = LogicalType(LogicalTypeId::INTEGER);
  const auto string = LogicalType(LogicalTypeId::STRING);
  const auto list = LogicalType::List(integer);
  const auto array = LogicalType::Array(string, 2);
  Schema schema({Column::Make("i", integer), Column::Make("s", string), Column::Make("items", list),
                 Column::Make("fixed", array)});

  const std::string long_string(4 * STANDARD_VECTOR_SIZE, 'x');
  data_chunk_vector_t chunks;
  chunks.push_back(OneChunk(
      schema.GetTypes(), {{Value(42), Value(long_string), Value::List(list, {Value(1), Value::Null(integer), Value(3)}),
                           Value::List(array, {Value("left"), Value("right")})},
                          {Value::Null(integer), Value::Null(string), Value::Null(list), Value::Null(array)}}));

  auto result = QueryResult::Rows(schema, std::move(chunks));
  ASSERT_EQ(result.RowCount(), 2U);
  EXPECT_EQ(result.Columns(), (std::vector<std::string>{"i", "s", "items", "fixed"}));
  EXPECT_EQ(result.Types(), schema.GetTypes());

  const auto rows = result.MaterializeRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 42);
  EXPECT_EQ(rows[0][1].GetString(), long_string);
  ASSERT_EQ(rows[0][2].GetChildren().size(), 3U);
  EXPECT_TRUE(rows[0][2].GetChildren()[1].IsNull());
  ASSERT_EQ(rows[0][3].GetChildren().size(), 2U);
  EXPECT_EQ(rows[0][3].GetChildren()[1].GetString(), "right");
  for (const auto &value : rows[1]) {
    EXPECT_TRUE(value.IsNull());
  }
}

TEST(QueryResultTest, PreservesEmptySchemaAndMultiChunkOrder) {
  const auto integer = LogicalType(LogicalTypeId::INTEGER);
  Schema schema({Column{"value", integer}});
  data_chunk_vector_t chunks;
  chunks.push_back(OneChunk(schema.GetTypes(), {{Value(1)}, {Value(2)}}));
  chunks.push_back(OneChunk(schema.GetTypes(), {{Value(3)}, {Value(4)}}));
  auto result = QueryResult::Rows(schema, std::move(chunks));

  EXPECT_EQ(result.RowCount(), 4U);
  const auto rows = result.MaterializeRows();
  for (idx_t i = 0; i < rows.size(); i++) {
    EXPECT_EQ(rows[i][0].GetAs<int32_t>(), static_cast<int32_t>(i + 1));
  }

  auto empty = QueryResult::Rows(schema, {});
  EXPECT_EQ(empty.RowCount(), 0U);
  EXPECT_EQ(empty.Columns(), (std::vector<std::string>{"value"}));
  EXPECT_EQ(empty.Types(), (std::vector<LogicalType>{integer}));
}

TEST(QueryResultTest, CommandMetadataAndDmlAffectedRowsAreTyped) {
  auto command = QueryResult::Command("CREATE TABLE", "created", 0, StatementType::CREATE_STATEMENT);
  EXPECT_TRUE(command.IsCommand());
  EXPECT_EQ(command.CommandTag(), "CREATE TABLE");
  EXPECT_EQ(command.Status(), "created");
  ASSERT_TRUE(command.AffectedRows().has_value());
  EXPECT_EQ(*command.AffectedRows(), 0);
  ASSERT_EQ(command.MaterializeRows().size(), 1U);
  EXPECT_EQ(command.MaterializeRows()[0][0].GetString(), "created");

  BumbleBeeInstance instance;
  auto results = instance.ExecuteSqlResults("CREATE TABLE t(v INT); INSERT INTO t VALUES (1),(2),(3);");
  ASSERT_EQ(results.size(), 2U);
  EXPECT_TRUE(results[0].IsCommand());
  ASSERT_TRUE(results[1].AffectedRows().has_value());
  EXPECT_EQ(*results[1].AffectedRows(), 3);
  EXPECT_EQ(results[1].MaterializeRows()[0][0].GetAs<int32_t>(), 3);
}

TEST(QueryResultTest, ScriptResultsStayInStatementOrder) {
  BumbleBeeInstance instance;
  auto results =
      instance.ExecuteSqlResults("CREATE TABLE t(v INT); INSERT INTO t VALUES (7),(9); SELECT v FROM t ORDER BY v;");
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0].GetStatementType(), StatementType::CREATE_STATEMENT);
  EXPECT_EQ(results[1].GetStatementType(), StatementType::INSERT_STATEMENT);
  EXPECT_EQ(results[2].GetStatementType(), StatementType::SELECT_STATEMENT);
  const auto rows = results[2].MaterializeRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 7);
  EXPECT_EQ(rows[1][0].GetAs<int32_t>(), 9);
}

TEST(QueryResultTest, ResultSurvivesExecutorTableAndInMemoryInstanceDestruction) {
  QueryResult result;
  {
    BumbleBeeInstance instance;
    instance.ExecuteSqlResults("CREATE TABLE t(v VARCHAR); INSERT INTO t VALUES ('owned payload');");
    auto selected = instance.ExecuteSqlResults("SELECT v FROM t;");
    result = std::move(selected.front());
    instance.ExecuteSqlResults("DROP TABLE t;");
  }

  const auto rows = result.MaterializeRows();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0][0].GetString(), "owned payload");
}

TEST(QueryResultTest, ResultSurvivesDurableDatabaseClose) {
  const auto path = std::filesystem::temp_directory_path() / "bbdb_query_result_ownership.db";
  std::filesystem::remove(path);
  QueryResult result;
  {
    BumbleBeeInstance instance(path, 32);
    instance.ExecuteSqlResults("CREATE TABLE t(v INT); INSERT INTO t VALUES (11),(12);");
    auto selected = instance.ExecuteSqlResults("SELECT v FROM t ORDER BY v;");
    result = std::move(selected.front());
  }
  std::filesystem::remove(path);

  const auto rows = result.MaterializeRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 11);
  EXPECT_EQ(rows[1][0].GetAs<int32_t>(), 12);
}

}  // namespace bumblebee
