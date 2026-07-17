//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// schema_codec_test.cpp
//
// Identification: test/unit/catalog/schema_codec_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "catalog/schema_codec.h"

#include <string>
#include <vector>

#include "common/byte_buffer.h"
#include "common/exception.h"
#include "gtest/gtest.h"

namespace bumblebee {

// A schema covering fixed, varlen, and parameterized (DECIMAL / DATE / TIMESTAMP) types round-trips
// through the codec: names, type ids, widths, and offsets all reconstruct identically.
TEST(SchemaCodecTest, RoundTripsAllColumnTypes) {
  std::vector<Column> cols{
      Column("i", LogicalType(LogicalTypeId::INTEGER)),
      Column("b", LogicalType(LogicalTypeId::BIGINT)),
      Column("name", LogicalType(LogicalTypeId::STRING), 128),
      Column("f", LogicalType(LogicalTypeId::FLOAT)),
      Column("dec", LogicalType::Decimal(LogicalType::MAX_DECIMAL_WIDTH_INT64, 3)),
      Column("dt", LogicalType(LogicalTypeId::DATE)),
      Column("ts", LogicalType(LogicalTypeId::TIMESTAMP)),
  };
  Schema original{cols};

  ByteWriter w;
  SerializeSchema(w, original);

  ByteReader r(w.Data().data(), w.Size());
  Schema restored = DeserializeSchema(r);
  EXPECT_EQ(r.Remaining(), 0U) << "the whole record was consumed";

  ASSERT_EQ(restored.GetColumnCount(), original.GetColumnCount());
  EXPECT_EQ(restored.GetInlinedStorageSize(), original.GetInlinedStorageSize());
  for (uint32_t i = 0; i < original.GetColumnCount(); i++) {
    const auto &oc = original.GetColumn(i);
    const auto &rc = restored.GetColumn(i);
    EXPECT_EQ(rc.GetName(), oc.GetName());
    EXPECT_EQ(rc.GetType().GetTypeId(), oc.GetType().GetTypeId());
    EXPECT_EQ(rc.GetStorageSize(), oc.GetStorageSize());
    EXPECT_EQ(rc.GetOffset(), oc.GetOffset());
  }
  // The DECIMAL width/scale survived.
  EXPECT_EQ(restored.GetColumn(4).GetType().GetDecimalData().width_, LogicalType::MAX_DECIMAL_WIDTH_INT64);
  EXPECT_EQ(restored.GetColumn(4).GetType().GetDecimalData().scale_, 3);
}

// A truncated record is rejected rather than read past the end of the buffer.
TEST(SchemaCodecTest, TruncatedRecordThrows) {
  Schema s{std::vector<Column>{Column("k", LogicalType(LogicalTypeId::BIGINT))}};
  ByteWriter w;
  SerializeSchema(w, s);
  // Feed only the first few bytes.
  ByteReader r(w.Data().data(), 2);
  EXPECT_THROW(DeserializeSchema(r), Exception);
}

}  // namespace bumblebee
