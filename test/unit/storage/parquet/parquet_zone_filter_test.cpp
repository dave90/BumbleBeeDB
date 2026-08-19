//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parquet_zone_filter_test.cpp
//
// Identification: test/unit/storage/parquet/parquet_zone_filter_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/parquet_zone_filter.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "storage/parquet/parquet_reader.h"
#include "storage/parquet/parquet_writer.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

namespace fs = std::filesystem;

static auto ColRef(uint32_t col, LogicalType type) -> AbstractExpressionRef {
  return std::make_shared<ColumnValueExpression>(0, col, Column::Make("c", type));
}

static auto Const(const Value &v) -> AbstractExpressionRef { return std::make_shared<ConstantValueExpression>(v); }

static auto Cmp(AbstractExpressionRef l, AbstractExpressionRef r, ComparisonType t) -> AbstractExpressionRef {
  return std::make_shared<ComparisonExpression>(std::move(l), std::move(r), t);
}

/** Zone predicates: extraction from expression trees, and pruning against real file statistics
 * written by our own ParquetWriter. */
class ParquetZoneFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("bumblebee_zone_" + std::to_string(::getpid()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  /** @brief Write one parquet file whose single INTEGER column holds [lo, hi]. */
  auto WriteRangeFile(const std::string &name, int32_t lo, int32_t hi) -> std::string {
    std::vector<LogicalType> types = {LogicalType(LogicalTypeId::INTEGER)};
    DataChunk chunk;
    chunk.Initialize(types);
    idx_t n = 0;
    for (int32_t v = lo; v <= hi; v++, n++) {
      chunk.SetValue(0, n, Value(v));
    }
    chunk.SetCardinality(n);
    ChunkCollection rows;
    rows.Append(chunk);
    auto path = (dir_ / name).string();
    ParquetWriter writer(path, types, {"a"}, format::CompressionCodec::UNCOMPRESSED);
    writer.Flush(rows);
    writer.Finalize();
    return path;
  }

  fs::path dir_;
};

TEST_F(ParquetZoneFilterTest, ExtractsConjunctsAndFlipsReversedComparisons) {
  auto int_type = LogicalType(LogicalTypeId::INTEGER);
  // (a > 5 AND 10 >= a) AND b = 'x'  -- the string equality also extracts; ORs would not.
  auto expr = std::make_shared<LogicExpression>(
      std::make_shared<LogicExpression>(Cmp(ColRef(0, int_type), Const(Value(5)), ComparisonType::GreaterThan),
                                        Cmp(Const(Value(10)), ColRef(0, int_type), ComparisonType::GreaterThanOrEqual),
                                        LogicType::And),
      Cmp(ColRef(1, LogicalType(LogicalTypeId::STRING)), Const(Value("x")), ComparisonType::Equal), LogicType::And);

  std::vector<ZonePredicate> preds;
  ExtractZonePredicates(expr, preds);
  ASSERT_EQ(preds.size(), 3u);
  EXPECT_EQ(preds[0].column_, 0u);
  EXPECT_EQ(preds[0].op_, ComparisonType::GreaterThan);
  // `10 >= a` flips to `a <= 10`.
  EXPECT_EQ(preds[1].column_, 0u);
  EXPECT_EQ(preds[1].op_, ComparisonType::LessThanOrEqual);
  EXPECT_EQ(preds[1].constant_.GetAs<int64_t>(), 10);
  EXPECT_EQ(preds[2].column_, 1u);
}

TEST_F(ParquetZoneFilterTest, OrNeverExtracts) {
  auto int_type = LogicalType(LogicalTypeId::INTEGER);
  auto expr = std::make_shared<LogicExpression>(Cmp(ColRef(0, int_type), Const(Value(5)), ComparisonType::Equal),
                                                Cmp(ColRef(0, int_type), Const(Value(9)), ComparisonType::Equal),
                                                LogicType::Or);
  std::vector<ZonePredicate> preds;
  ExtractZonePredicates(expr, preds);
  EXPECT_TRUE(preds.empty());
}

TEST_F(ParquetZoneFilterTest, PrunesAgainstWrittenStatistics) {
  auto path = WriteRangeFile("r.parquet", 10, 20);  // stats: min 10, max 20
  ParquetReader reader(GlobalParquetAllocator(), path);
  const auto &group = reader.GetFileMetadata()->row_groups[0];
  const auto &types = reader.return_types_;

  auto check = [&](ComparisonType op, int32_t c) {
    return RowGroupCanMatch(group, types, {ZonePredicate{0, op, Value(c)}});
  };

  // Equal.
  EXPECT_TRUE(check(ComparisonType::Equal, 10));
  EXPECT_TRUE(check(ComparisonType::Equal, 15));
  EXPECT_FALSE(check(ComparisonType::Equal, 9));
  EXPECT_FALSE(check(ComparisonType::Equal, 21));
  // Range operators at the boundaries.
  EXPECT_FALSE(check(ComparisonType::GreaterThan, 20));
  EXPECT_TRUE(check(ComparisonType::GreaterThanOrEqual, 20));
  EXPECT_FALSE(check(ComparisonType::LessThan, 10));
  EXPECT_TRUE(check(ComparisonType::LessThanOrEqual, 10));
  // Several predicates: any one proving emptiness prunes.
  EXPECT_FALSE(RowGroupCanMatch(group, types,
                                {ZonePredicate{0, ComparisonType::GreaterThan, Value(5)},
                                 ZonePredicate{0, ComparisonType::GreaterThan, Value(50)}}));
}

TEST_F(ParquetZoneFilterTest, MissingStatsNeverPrune) {
  // A foreign corpus file column with strings: no numeric stats -> never pruned.
  fs::path corpus = fs::path(__FILE__).parent_path() / "data" / "t1.parquet";
  ParquetReader reader(GlobalParquetAllocator(), corpus.string());
  const auto &group = reader.GetFileMetadata()->row_groups[0];
  // The string column (idx 1) with an Equal predicate: stats exist or not, string predicates are
  // out of the numeric zone-map domain and must not prune.
  EXPECT_TRUE(RowGroupCanMatch(group, reader.return_types_, {ZonePredicate{1, ComparisonType::Equal, Value("zzz")}}));
}

}  // namespace bumblebee
