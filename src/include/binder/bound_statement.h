//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_statement.h
//
// Identification: src/include/binder/bound_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "common/enums/statement_type.h"
#include "common/exception.h"

namespace bumblebee {

/**
 * The base class of every bound SQL statement.
 */
class BoundStatement {
 public:
  /**
   * @brief Construct a bound statement of the given kind.
   *
   * @param type The statement type.
   */
  explicit BoundStatement(StatementType type);

  virtual ~BoundStatement() = default;

  /** The statement type. */
  StatementType type_;

  /** @return A human-readable rendering of this statement. */
  virtual auto ToString() const -> std::string {
    throw Exception("ToString not supported for this type of statement");
  }
};

}  // namespace bumblebee
