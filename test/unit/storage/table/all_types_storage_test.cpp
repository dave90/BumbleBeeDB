//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// all_types_storage_test.cpp
//
// Identification: test/unit/storage/table/all_types_storage_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "gtest/gtest.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"
#include "storage/row/row_layout.h"
#include "storage/row/row_operations.h"
#include "storage/table/table_heap.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/vector.h"

namespace bumblebee {

// Every physical representation the row-storage engine supports, plus the logical types that reduce
// to them (BOOLEAN→TINYINT, DECIMAL→BIGINT, DATE→INTEGER, TIMESTAMP→BIGINT). Row bytes are entirely
// determined by the physical type, so covering every width here covers every logical type that maps
// to it — and it exercises the same Scatter/FullScan path the MVCC engine uses.
static auto AllTypesSchema() -> SchemaRef {
  std::vector<Column> cols{
      Column("b", LogicalType(LogicalTypeId::BOOLEAN)),
      Column("i8", LogicalType(LogicalTypeId::TINYINT)),
      Column("i16", LogicalType(LogicalTypeId::SMALLINT)),
      Column("i32", LogicalType(LogicalTypeId::INTEGER)),
      Column("i64", LogicalType(LogicalTypeId::BIGINT)),
      Column("u8", LogicalType(LogicalTypeId::UTINYINT)),
      Column("u16", LogicalType(LogicalTypeId::USMALLINT)),
      Column("u32", LogicalType(LogicalTypeId::UINTEGER)),
      Column("u64", LogicalType(LogicalTypeId::UBIGINT)),
      Column("f", LogicalType(LogicalTypeId::FLOAT)),
      Column("d", LogicalType(LogicalTypeId::DOUBLE)),
      Column("s", LogicalType(LogicalTypeId::STRING), VARCHAR_DEFAULT_LENGTH),
      Column("dec", LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, 2)),  // physically BIGINT
      Column("dt", LogicalType(LogicalTypeId::DATE)),                                // physically INTEGER
      Column("ts", LogicalType(LogicalTypeId::TIMESTAMP)),                           // physically BIGINT
  };
  return std::make_shared<Schema>(cols);
}

// Round-trip a DataChunk holding one column of every supported type through TableHeap append + scan
// (the real storage path). Covers distinctive values, boundary min/max per width, and a NULL in every
// column (per-type validity), asserting cell-for-cell equality after the round-trip.
TEST(AllTypesStorageTest, EveryTypeRoundTripsThroughTableHeap) {
  MemoryDiskManager dm(256);
  BufferPoolManager bpm(16, &dm);
  auto schema = AllTypesSchema();
  TableHeap heap(&bpm, schema);
  const auto types = schema->GetTypes();

  DataChunk in;
  in.Initialize(types);

  // Row 0: ordinary distinctive values.
  int col = 0;
  in.SetValue(col++, 0, Value(true));
  in.SetValue(col++, 0, Value(static_cast<int8_t>(-12)));
  in.SetValue(col++, 0, Value(static_cast<int16_t>(-1234)));
  in.SetValue(col++, 0, Value(static_cast<int32_t>(-123456)));
  in.SetValue(col++, 0, Value(static_cast<int64_t>(-1234567890123)));
  in.SetValue(col++, 0, Value(static_cast<uint8_t>(200)));
  in.SetValue(col++, 0, Value(static_cast<uint16_t>(60000)));
  in.SetValue(col++, 0, Value(static_cast<uint32_t>(4000000000U)));
  in.SetValue(col++, 0, Value(static_cast<uint64_t>(18000000000000000000ULL)));
  in.SetValue(col++, 0, Value(static_cast<float>(1.5F)));
  in.SetValue(col++, 0, Value(static_cast<double>(2.5)));
  in.SetValue(col++, 0, Value(std::string("hello types")));
  in.SetValue(col++, 0, Value(static_cast<int64_t>(12345)));             // DECIMAL backing (123.45)
  in.SetValue(col++, 0, Value(static_cast<int32_t>(19000)));             // DATE (days since epoch)
  in.SetValue(col++, 0, Value(static_cast<int64_t>(1700000000000000)));  // TIMESTAMP (micros)

  // Row 1: a NULL in every column (per-type validity prefix must survive).
  for (idx_t j = 0; j < types.size(); j++) {
    in.SetValue(j, 1, Value::Null(types[j]));
  }

  // Row 2: boundary min/max per width.
  col = 0;
  in.SetValue(col++, 2, Value(false));
  in.SetValue(col++, 2, Value(std::numeric_limits<int8_t>::min()));
  in.SetValue(col++, 2, Value(std::numeric_limits<int16_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<int32_t>::min()));
  in.SetValue(col++, 2, Value(std::numeric_limits<int64_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<uint8_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<uint16_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<uint32_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<uint64_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<float>::lowest()));
  in.SetValue(col++, 2, Value(std::numeric_limits<double>::max()));
  in.SetValue(col++, 2, Value(std::string("")));  // empty string
  in.SetValue(col++, 2, Value(std::numeric_limits<int64_t>::min()));
  in.SetValue(col++, 2, Value(std::numeric_limits<int32_t>::max()));
  in.SetValue(col++, 2, Value(std::numeric_limits<int64_t>::min()));
  in.SetCardinality(3);

  Vector rids{LogicalType{LogicalTypeId::BIGINT}};
  heap.Append(in, rids);

  auto scan = heap.MakeScan();
  DataChunk out;
  out.Initialize(types);
  ASSERT_TRUE(scan->Next(out));
  ASSERT_EQ(out.GetSize(), 3U);

  for (idx_t i = 0; i < 3; i++) {
    for (idx_t j = 0; j < types.size(); j++) {
      auto expected = in.GetValue(j, i);
      auto actual = out.GetValue(j, i);
      EXPECT_EQ(actual.IsNull(), expected.IsNull()) << "null mismatch at row " << i << " col " << j;
      if (!expected.IsNull()) {
        EXPECT_EQ(actual, expected) << "value mismatch at row " << i << " col " << j;
      }
    }
  }
}

// Row storage does not yet support nested types (STRUCT/LIST/ARRAY): the row↔chunk kernel rejects
// them with NotImplementedException. (The catalog `Column` layer rejects them even earlier, via an
// assertion, so a nested column never reaches storage in normal use — here we drive RowOperations
// directly to pin the storage-layer boundary itself.) This keeps the limit a visible, deliberate
// boundary rather than a silent corruption.
void ExpectScatterRejectsNested(const LogicalType &nested) {
  std::vector<LogicalType> types{LogicalType(LogicalTypeId::INTEGER), nested};
  DataChunk in;
  in.Initialize(types);
  in.SetValue(0, 0, Value(static_cast<int32_t>(1)));
  in.SetValue(1, 0, Value::List(nested, {Value(static_cast<int32_t>(3)), Value(static_cast<int32_t>(4))}));
  in.SetCardinality(1);

  RowLayout layout;
  layout.Initialize(types);
  std::vector<char> slot(layout.GetFixedRowWidth() + 64, 0);
  Vector rows{LogicalType{LogicalTypeId::UBIGINT}};
  FlatVector::GetData<data_ptr_t>(rows)[0] = reinterpret_cast<data_ptr_t>(slot.data());

  SelectionVector identity;
  EXPECT_THROW(RowOperations::Scatter(in, layout, rows, identity, 1), NotImplementedException);
}

TEST(AllTypesStorageTest, ListColumnRejectedByRowStorage) {
  ExpectScatterRejectsNested(LogicalType::List(LogicalType(LogicalTypeId::INTEGER)));
}

TEST(AllTypesStorageTest, ArrayColumnRejectedByRowStorage) {
  ExpectScatterRejectsNested(LogicalType::Array(LogicalType(LogicalTypeId::INTEGER), 2));
}

}  // namespace bumblebee
