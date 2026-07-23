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
  }
  return true;
}

}  // namespace bumblebee
