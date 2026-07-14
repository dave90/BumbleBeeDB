//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// statement_type.h
//
// Identification: src/include/common/enums/statement_type.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "fmt/format.h"

namespace bumblebee {

/** The kinds of SQL statement BumbleBeeDB can bind. */
enum class StatementType : uint8_t {
  /** Invalid statement. */
  INVALID_STATEMENT,
  /** SELECT. */
  SELECT_STATEMENT,
  /** INSERT. */
  INSERT_STATEMENT,
  /** UPDATE. */
  UPDATE_STATEMENT,
  /** DELETE. */
  DELETE_STATEMENT,
  /** CREATE TABLE. */
  CREATE_STATEMENT,
  /** EXPLAIN. */
  EXPLAIN_STATEMENT,
};

/**
 * @brief Render a statement type as a human-readable string.
 *
 * @param type The statement type.
 * @return std::string The human-readable name.
 */
auto StatementTypeToString(StatementType type) -> std::string;

}  // namespace bumblebee

template <>
struct fmt::formatter<bumblebee::StatementType> : formatter<std::string> {
  template <typename FormatCtx>
  auto format(bumblebee::StatementType type, FormatCtx &ctx) const {
    return formatter<std::string>::format(bumblebee::StatementTypeToString(type), ctx);
  }
};
