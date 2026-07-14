//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// select_statement.h
//
// Identification: src/include/binder/statement/select_statement.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/bound_order_by.h"
#include "binder/bound_statement.h"
#include "binder/bound_table_ref.h"
#include "binder/table_ref/bound_subquery_ref.h"

namespace bumblebee {

/**
 * A bound SELECT statement.
 *
 * Absent clauses are represented by a default-constructed (INVALID) BoundExpression
 * rather than by a null pointer, so that ToString() never has to special-case them.
 */
class SelectStatement : public BoundStatement {
 public:
  /**
   * @brief Construct a bound SELECT.
   *
   * @param table The bound FROM clause.
   * @param select_list The bound SELECT list.
   * @param where The bound WHERE clause.
   * @param group_by The bound GROUP BY clause.
   * @param having The bound HAVING clause.
   * @param limit_count The bound LIMIT clause.
   * @param limit_offset The bound OFFSET clause.
   * @param sort The bound ORDER BY clause.
   * @param ctes The bound CTEs.
   * @param is_distinct True for SELECT DISTINCT.
   */
  explicit SelectStatement(std::unique_ptr<BoundTableRef> table,
                           std::vector<std::unique_ptr<BoundExpression>> select_list,
                           std::unique_ptr<BoundExpression> where,
                           std::vector<std::unique_ptr<BoundExpression>> group_by,
                           std::unique_ptr<BoundExpression> having, std::unique_ptr<BoundExpression> limit_count,
                           std::unique_ptr<BoundExpression> limit_offset,
                           std::vector<std::unique_ptr<BoundOrderBy>> sort, CTEList ctes, bool is_distinct)
      : BoundStatement(StatementType::SELECT_STATEMENT),
        table_(std::move(table)),
        select_list_(std::move(select_list)),
        where_(std::move(where)),
        group_by_(std::move(group_by)),
        having_(std::move(having)),
        limit_count_(std::move(limit_count)),
        limit_offset_(std::move(limit_offset)),
        sort_(std::move(sort)),
        ctes_(std::move(ctes)),
        is_distinct_(is_distinct) {}

  /** The bound FROM clause. */
  std::unique_ptr<BoundTableRef> table_;

  /** The bound SELECT list. */
  std::vector<std::unique_ptr<BoundExpression>> select_list_;

  /** The bound WHERE clause. */
  std::unique_ptr<BoundExpression> where_;

  /** The bound GROUP BY clause. */
  std::vector<std::unique_ptr<BoundExpression>> group_by_;

  /** The bound HAVING clause. */
  std::unique_ptr<BoundExpression> having_;

  /** The bound LIMIT clause. */
  std::unique_ptr<BoundExpression> limit_count_;

  /** The bound OFFSET clause. */
  std::unique_ptr<BoundExpression> limit_offset_;

  /** The bound ORDER BY clause. */
  std::vector<std::unique_ptr<BoundOrderBy>> sort_;

  /** The bound CTEs. */
  CTEList ctes_;

  /** True for SELECT DISTINCT. */
  bool is_distinct_;

  auto ToString() const -> std::string override;
};

}  // namespace bumblebee
