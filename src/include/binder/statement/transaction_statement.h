//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// transaction_statement.h
//
// Identification: src/include/binder/statement/transaction_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

#include "binder/bound_statement.h"

namespace bumblebee {

/** The explicit transaction-control commands the shell understands. */
enum class TransactionType : uint8_t {
  /** BEGIN / START TRANSACTION: open an explicit transaction. */
  BEGIN,
  /** COMMIT / END: publish the explicit transaction's writes. */
  COMMIT,
  /** ROLLBACK / ABORT: discard the explicit transaction's writes. */
  ROLLBACK,
};

/** @brief Render a transaction command as its SQL keyword (`BEGIN` / `COMMIT` / `ROLLBACK`). */
auto TransactionTypeToString(TransactionType type) -> std::string;

/**
 * A bound transaction-control statement (BEGIN / COMMIT / ROLLBACK). It carries no operands — the
 * effect depends only on whether the session currently holds an explicit transaction.
 */
class TransactionStatement : public BoundStatement {
 public:
  /** @brief Construct a bound transaction command. */
  explicit TransactionStatement(TransactionType txn_type);

  /** Which transaction command this is. */
  TransactionType txn_type_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
