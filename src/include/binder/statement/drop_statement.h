//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// drop_statement.h
//
// Identification: src/include/binder/statement/drop_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "binder/bound_statement.h"

namespace bumblebee {

/**
 * A bound DROP TABLE statement. May name several tables (`DROP TABLE a, b`); `if_exists_` is set by
 * `DROP TABLE IF EXISTS`, which turns a missing table into a no-op instead of an error.
 */
class DropStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound DROP TABLE.
   *
   * @param tables The names of the tables to drop.
   * @param if_exists When true, a missing table is not an error.
   */
  DropStatement(std::vector<std::string> tables, bool if_exists);

  /** The tables to drop. */
  std::vector<std::string> tables_;

  /** When true (`IF EXISTS`), dropping a missing table is a no-op rather than an error. */
  bool if_exists_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
