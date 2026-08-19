//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// physical_parquet_scan_test.cpp
//
// Identification: test/unit/execution/operator/scan/physical_parquet_scan_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <unistd.h>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/exception.h"
#include "execution/operator/scan/physical_parquet_scan.h"
#include "gtest/gtest.h"
#include "operator_test_util.h"
#include "storage/parquet/parquet_manifest.h"
#include "storage/parquet/parquet_table_ops.h"
#include "type/vector/chunk_collection.h"

namespace bumblebee {

namespace fs = std::filesystem;

namespace {

const LogicalType kInt(LogicalTypeId::INTEGER);
const LogicalType kBigint(LogicalTypeId::BIGINT);

/**
 * @brief Fixture: a table folder with caller-written part files and a manifest, registered in the
 * harness catalog as an external parquet table.
 *
 * The scan's snapshot is the manifest, so tests drive it exactly the way the write path commits:
 * part files + an atomically-named `_bbdb_manifest.N`.
 */
class PhysicalParquetScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("bumblebee_pq_scan_" + std::to_string(::getpid()) + "_" +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }

  void TearDown() override { fs::remove_all(dir_); }

  /** @brief Write each row batch as its own part file, commit one manifest listing them all, and
   * register the table (declared as `schema`) in the catalog. Returns the table oid. */
  auto MakeTable(const SchemaRef &schema, const std::vector<std::vector<TestRow>> &parts,
                 const SchemaRef &file_schema = nullptr) -> table_oid_t {
    const auto &write_schema = file_schema != nullptr ? file_schema : schema;
    ParquetManifest manifest;
    manifest.version_ = 0;
    for (size_t p = 0; p < parts.size(); p++) {
      ChunkCollection rows;
      auto chunk = std::make_unique<DataChunk>();
      chunk->Initialize(write_schema->GetTypes());
      for (idx_t i = 0; i < parts[p].size(); i++) {
        for (idx_t c = 0; c < parts[p][i].size(); c++) {
          chunk->SetValue(c, i, parts[p][i][c]);
        }
      }
      chunk->SetCardinality(parts[p].size());
      rows.Append(std::move(chunk));
      const auto name = "part-" + std::to_string(p) + ".parquet";
      manifest.entries_.push_back(WritePartFile(dir_.string(), name, *write_schema, rows));
    }
    ParquetManifestIO::Write(dir_.string(), manifest);

    auto info = h_.catalog->CreateTable("t", *schema, StorageFormat::PARQUET, {}, false, dir_.string());
    EXPECT_NE(info, NULL_TABLE_INFO);
    return info->oid_;
  }

  auto RunScan(std::unique_ptr<PhysicalOperator> scan) -> std::vector<TestRow> {
    RowCollector collector(std::move(scan));
    auto rows = h_.Run(collector);
    SortRows(rows);
    return rows;
  }

  OperatorHarness h_;
  fs::path dir_;
};

auto TwoIntSchema() -> SchemaRef { return MakeSchemaOf({{"k", kInt}, {"v", kInt}}); }
auto Row(int32_t k, int32_t v) -> TestRow { return {Value(k), Value(v)}; }

}  // namespace

TEST_F(PhysicalParquetScanTest, ReturnsEveryRowFromOnePartFile) {
  auto schema = TwoIntSchema();
  auto oid = MakeTable(schema, {{Row(1, 10), Row(2, 20), Row(3, 30)}});
  auto rows = RunScan(std::make_unique<PhysicalParquetScan>(schema, oid, "t", 3));
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0][0].GetAs<int32_t>(), 1);
  EXPECT_EQ(rows[2][1].GetAs<int32_t>(), 30);
}

// One morsel = one (file, row group): several part files must all be claimed and scanned.
TEST_F(PhysicalParquetScanTest, ScansEveryPartFile) {
  auto schema = TwoIntSchema();
  std::vector<std::vector<TestRow>> parts(3);
  for (int p = 0; p < 3; p++) {
    for (int i = 0; i < 20; i++) {
      parts[p].push_back(Row(p * 100 + i, i));
    }
  }
  auto oid = MakeTable(schema, parts);
  auto rows = RunScan(std::make_unique<PhysicalParquetScan>(schema, oid, "t", 60));
  ASSERT_EQ(rows.size(), 60U);
  std::set<int32_t> ks;
  for (const auto &r : rows) {
    ks.insert(r[0].GetAs<int32_t>());
  }
  EXPECT_EQ(ks.size(), 60U) << "no row dropped or duplicated across file morsels";
}

TEST_F(PhysicalParquetScanTest, EmptyManifestProducesNoRows) {
  auto schema = TwoIntSchema();
  auto oid = MakeTable(schema, {});
  EXPECT_TRUE(RunScan(std::make_unique<PhysicalParquetScan>(schema, oid, "t", 0)).empty());
}

// The column-pruning contract, parquet flavor: unprojected columns' pages are never decoded and
// surface as constant-NULL in a still-full-width chunk.
TEST_F(PhysicalParquetScanTest, ProjectionLeavesPrunedColumnsNull) {
  auto schema = TwoIntSchema();
  auto oid = MakeTable(schema, {{Row(1, 10), Row(2, 20)}});
  auto rows = RunScan(std::make_unique<PhysicalParquetScan>(schema, oid, "t", 2, /*emit_rids=*/false,
                                                            /*predicate=*/nullptr, std::vector<idx_t>{1}));
  ASSERT_EQ(rows.size(), 2U);
  std::multiset<int32_t> vs;
  for (const auto &r : rows) {
    ASSERT_EQ(r.size(), 2U) << "schema stays full-width";
    EXPECT_TRUE(r[0].IsNull()) << "the pruned column reads as NULL";
    vs.insert(r[1].GetAs<int32_t>());
  }
  EXPECT_EQ(vs, (std::multiset<int32_t>{10, 20}));
}

// RID = (file_idx << 32) | row_in_file — the identifier the external write operators resolve
// against the same manifest snapshot.
TEST_F(PhysicalParquetScanTest, EmitRidsEncodesFileAndRow) {
  auto schema = TwoIntSchema();
  auto oid = MakeTable(schema, {{Row(1, 10), Row(2, 20)}, {Row(3, 30)}});
  auto rid_schema = MakeSchemaOf({{"k", kInt}, {"v", kInt}, {"__rid", kBigint}});
  auto rows = RunScan(std::make_unique<PhysicalParquetScan>(rid_schema, oid, "t", 3, /*emit_rids=*/true));
  ASSERT_EQ(rows.size(), 3U);
  std::set<int64_t> rids;
  for (const auto &r : rows) {
    ASSERT_FALSE(r[2].IsNull());
    rids.insert(r[2].GetAs<int64_t>());
  }
  EXPECT_EQ(rids, (std::set<int64_t>{0, 1, (int64_t{1} << 32) | 0}));
}

namespace {

/** @brief The externally-written INT32-stored decimal fixture (BumbleBee's own writer emits
 * decimals as DOUBLE, so this storage layout can only come from an external file): one column
 * `value`, 24 rows, 1.00 .. 24.00 — raw unscaled 100 .. 2400, all positive. */
auto Int32DecimalFixture() -> std::string {
  const fs::path here = __FILE__;
  return (here.parent_path() / "../../../storage/parquet/data/int32_decimal.parquet").string();
}

}  // namespace

// Writing this test surfaced a bug beyond the W12 clamp: the column reader decodes into the
// backing integer selected by the FILE's decimal width, while the scan's output vector uses the
// TABLE's declared width — and `ExternalSchemaMatches` compared DECIMALs by LogicalTypeId only,
// so an INT32-stored file read through a DECIMAL(18,2) declaration type-punned 24 int32 payloads
// into an int64 buffer (12 packed garbage values + 12 zeros; the mirror case, a NARROWER
// declaration, heap-overflows once a full chunk lands). The schema guard now requires the same
// physical backing, so the mismatch fails loudly at scan open — the exact contract its docstring
// promises ("fail loudly instead of decoding garbage").
TEST_F(PhysicalParquetScanTest, DecimalBackingWidthMismatchIsRejectedAtScanOpen) {
  fs::copy_file(Int32DecimalFixture(), dir_ / "part-0.parquet");
  ParquetManifest manifest;
  manifest.version_ = 0;
  manifest.entries_.push_back(ManifestEntry{"part-0.parquet", 24});
  ParquetManifestIO::Write(dir_.string(), manifest);

  auto declared = MakeSchemaOf({{"value", LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, 2)}});
  auto info = h_.catalog->CreateTable("t", *declared, StorageFormat::PARQUET, {}, false, dir_.string());
  ASSERT_NE(info, NULL_TABLE_INFO);

  EXPECT_THROW(RunScan(std::make_unique<PhysicalParquetScan>(declared, info->oid_, "t", 24)), ExecutionException);
}

// The widths-agree baseline the corpus exercises: declared DECIMAL(9,2) backs onto INT32, same
// as the file's storage — bit-identical before and after the W12 fix, negatives included.
TEST_F(PhysicalParquetScanTest, DecimalMatchingWidthRoundTrips) {
  fs::copy_file(Int32DecimalFixture(), dir_ / "part-0.parquet");
  ParquetManifest manifest;
  manifest.version_ = 0;
  manifest.entries_.push_back(ManifestEntry{"part-0.parquet", 24});
  ParquetManifestIO::Write(dir_.string(), manifest);

  auto declared = MakeSchemaOf({{"value", LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT32, 2)}});
  auto info = h_.catalog->CreateTable("t", *declared, StorageFormat::PARQUET, {}, false, dir_.string());
  ASSERT_NE(info, NULL_TABLE_INFO);

  auto rows = RunScan(std::make_unique<PhysicalParquetScan>(declared, info->oid_, "t", 24));
  ASSERT_EQ(rows.size(), 24U);
  std::multiset<int32_t> raw;
  for (const auto &r : rows) {
    raw.insert(r[0].GetAs<int32_t>());
  }
  EXPECT_EQ(*raw.begin(), 100);
  EXPECT_EQ(*raw.rbegin(), 2400);
  EXPECT_EQ(raw.size(), 24U);
}

}  // namespace bumblebee
