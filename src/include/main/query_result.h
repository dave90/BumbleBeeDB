//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// query_result.h
//
// Identification: src/include/main/query_result.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/enums/statement_type.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"

namespace bumblebee {

/** Whether a result contains relational rows or command/status metadata only. */
enum class QueryResultKind : uint8_t { ROWS, COMMAND };

/**
 * @brief A typed, immutable, owning result detached from an executor and database lifetime.
 *
 * Result chunks are normalized before they enter this object, so every vector owns its fixed-width,
 * string, validity, and nested child storage. Column names and logical types are copied from the
 * physical root. No member points into a Catalog, table, transaction, Connection, or frontend.
 */
class QueryResult {
 public:
  QueryResult() = default;
  QueryResult(QueryResult &&) noexcept = default;
  auto operator=(QueryResult &&) noexcept -> QueryResult & = default;
  QueryResult(const QueryResult &) = delete;
  auto operator=(const QueryResult &) -> QueryResult & = delete;

  /**
   * @brief Build an owning row result from a schema and normalized chunks.
   * @param schema The result schema to copy.
   * @param chunks The result chunks to take ownership of.
   * @param statement_type The statement that produced the rows.
   * @return QueryResult The detached result.
   */
  static auto Rows(const Schema &schema, data_chunk_vector_t chunks,
                   StatementType statement_type = StatementType::SELECT_STATEMENT) -> QueryResult;

  /**
   * @brief Build a command result with status metadata and no relational rows.
   * @param command_tag Stable command name, such as `CREATE TABLE` or `COMMIT`.
   * @param status Human-readable frontend status.
   * @param affected_rows Optional affected-row count.
   * @return QueryResult The command result.
   */
  static auto Command(std::string command_tag, std::string status, std::optional<int64_t> affected_rows = std::nullopt,
                      StatementType statement_type = StatementType::INVALID_STATEMENT) -> QueryResult;

  /** @return Whether this is a command/status result rather than a row result. */
  auto IsCommand() const -> bool { return kind_ == QueryResultKind::COMMAND; }
  /** @return The statement type that produced this result. */
  auto GetStatementType() const -> StatementType { return statement_type_; }
  /** @return The copied output column names, including duplicates. */
  auto Columns() const -> const std::vector<std::string> & { return columns_; }
  /** @return The copied logical output types. */
  auto Types() const -> const std::vector<LogicalType> & { return types_; }
  /** @return The immutable, owned result chunks. */
  auto Chunks() const -> const data_chunk_vector_t & { return chunks_; }
  /** @return The number of relational rows, cached at construction. */
  auto RowCount() const -> idx_t { return row_count_; }
  /** @return The command tag, empty for ordinary row results. */
  auto CommandTag() const -> const std::string & { return command_tag_; }
  /** @return The human-readable command status, empty for ordinary row results. */
  auto Status() const -> const std::string & { return status_; }
  /** @return The affected-row count when the producer reports one. */
  auto AffectedRows() const -> const std::optional<int64_t> & { return affected_rows_; }

  /**
   * @brief Copy the result into boxed native values for a frontend boundary.
   * @return Rows in result order. The returned Values own all nested/string payloads.
   */
  auto MaterializeRows() const -> std::vector<std::vector<Value>>;

 private:
  QueryResultKind kind_{QueryResultKind::ROWS};
  StatementType statement_type_{StatementType::INVALID_STATEMENT};
  std::vector<std::string> columns_;
  std::vector<LogicalType> types_;
  data_chunk_vector_t chunks_;
  idx_t row_count_{0};
  std::string command_tag_;
  std::string status_;
  std::optional<int64_t> affected_rows_;
};

}  // namespace bumblebee
