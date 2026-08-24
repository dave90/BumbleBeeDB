//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// dataframe_load_test.cpp
//
// Identification: test/unit/main/dataframe_load_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database_config.h"
#include "main/database_instance.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {
namespace {

auto MakeChunks(const std::vector<LogicalType> &types, const std::vector<std::vector<Value>> &rows)
    -> data_chunk_vector_t {
  data_chunk_vector_t chunks;
  for (idx_t offset = 0; offset < rows.size(); offset += STANDARD_VECTOR_SIZE) {
    auto chunk = std::make_unique<DataChunk>();
    chunk->Initialize(types);
    const auto count = std::min<idx_t>(STANDARD_VECTOR_SIZE, rows.size() - offset);
    for (idx_t row = 0; row < count; row++) {
      for (idx_t column = 0; column < types.size(); column++) {
        chunk->SetValue(column, row, rows[offset + row][column]);
      }
    }
    chunk->SetCardinality(count);
    chunks.push_back(std::move(chunk));
  }
  return chunks;
}

auto TestDatabase() -> std::shared_ptr<DatabaseInstance> {
  DatabaseConfig config;
  config.frames_ = 128;
  return std::make_shared<DatabaseInstance>(config);
}

TEST(DataFrameLoadTest, NativeChunksUseAutoIdAndNormalInsertPath) {
  auto database = TestDatabase();
  auto connection = DatabaseInstance::CreateConnection(database);
  const std::vector<LogicalType> types{LogicalTypeId::INTEGER, LogicalTypeId::STRING};

  EXPECT_EQ(connection->LoadDataChunks("loaded", {"value", "label"}, types, {}, false,
                                       MakeChunks(types, {{Value(7), Value("seven")}, {Value(11), Value("eleven")}})),
            2U);
  auto rows = connection->ExecuteSqlStatement("SELECT _id, value, label FROM loaded ORDER BY _id").MaterializeRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0][0].GetAs<int64_t>(), 0);
  EXPECT_EQ(rows[0][1].GetAs<int32_t>(), 7);
  EXPECT_EQ(rows[0][2].GetString(), "seven");
  EXPECT_EQ(rows[1][0].GetAs<int64_t>(), 1);
}

TEST(DataFrameLoadTest, FailedCreateDropsThePartiallyLoadedTable) {
  auto database = TestDatabase();
  auto connection = DatabaseInstance::CreateConnection(database);
  const std::vector<LogicalType> types{LogicalTypeId::BIGINT, LogicalTypeId::STRING};

  EXPECT_THROW(connection->LoadDataChunks(
                   "duplicate", {"id", "label"}, types, {"id"}, false,
                   MakeChunks(types, {{Value(int64_t{1}), Value("first")}, {Value(int64_t{1}), Value("duplicate")}})),
               ConflictException);
  EXPECT_EQ(database->GetCatalog().GetTable("duplicate"), nullptr);
}

TEST(DataFrameLoadTest, AppendValidatesFirstAndRollsBackTheWholeBatch) {
  auto database = TestDatabase();
  auto connection = DatabaseInstance::CreateConnection(database);
  const std::vector<LogicalType> types{LogicalTypeId::BIGINT, LogicalTypeId::STRING};
  connection->LoadDataChunks("target", {"id", "label"}, types, {"id"}, false,
                             MakeChunks(types, {{Value(int64_t{1}), Value("one")}, {Value(int64_t{2}), Value("two")}}));

  EXPECT_THROW(connection->LoadDataChunks(
                   "target", {"id", "label"}, types, {"id"}, true,
                   MakeChunks(types, {{Value(int64_t{3}), Value("three")}, {Value(int64_t{2}), Value("duplicate")}})),
               ConflictException);

  const std::vector<LogicalType> wrong_types{LogicalTypeId::INTEGER, LogicalTypeId::STRING};
  EXPECT_THROW(connection->LoadDataChunks("target", {"id", "label"}, wrong_types, {}, true,
                                          MakeChunks(wrong_types, {{Value(4), Value("four")}})),
               DataException);

  auto rows = connection->ExecuteSqlStatement("SELECT id, label FROM target ORDER BY id").MaterializeRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0][0].GetAs<int64_t>(), 1);
  EXPECT_EQ(rows[1][0].GetAs<int64_t>(), 2);
}

}  // namespace
}  // namespace bumblebee
