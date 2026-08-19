//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// external_schema.h
//
// Identification: src/include/storage/parquet/external_schema.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/util/string_util.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * @brief True when a parquet file's schema (names + types, in order) matches the catalog schema
 * of an external table. Names compare case-insensitively; types by LogicalTypeId.
 *
 * Used at CREATE (declared-schema validation / multi-file inference agreement) and again at scan
 * open, so files replaced externally with a different schema fail loudly instead of decoding
 * garbage.
 */
inline auto ExternalSchemaMatches(const Schema &expected, const std::vector<std::string> &file_names,
                                  const std::vector<LogicalType> &file_types) -> bool {
  if (expected.GetColumnCount() != file_names.size()) {
    return false;
  }
  for (idx_t i = 0; i < file_names.size(); i++) {
    const auto &col = expected.GetColumn(i);
    if (StringUtil::Lower(col.GetName()) != StringUtil::Lower(file_names[i])) {
      return false;
    }
    if (col.GetType().GetTypeId() != file_types[i].GetTypeId()) {
      return false;
    }
    // DECIMAL: id equality is not enough. The column reader decodes into the backing integer
    // selected by the FILE's width (the reader is built from the footer), while the scan's output
    // vector is allocated with the TABLE's declared width — if the two backings differ, the raw
    // payload is type-punned into the wrong-width buffer (garbage values; a heap overflow once a
    // full chunk of wider values lands in a narrower vector). Requiring the same physical backing
    // makes that fail loudly here instead.
    if (col.GetType().GetTypeId() == LogicalTypeId::DECIMAL &&
        col.GetType().GetPhysicalType() != file_types[i].GetPhysicalType()) {
      return false;
    }
  }
  return true;
}

}  // namespace bumblebee
