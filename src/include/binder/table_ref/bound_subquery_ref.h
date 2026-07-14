//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_subquery_ref.h
//
// Identification: src/include/binder/table_ref/bound_subquery_ref.h
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
#include "binder/bound_table_ref.h"
#include "fmt/format.h"

namespace bumblebee {

class SelectStatement;

/**
 * A subquery in a FROM clause, e.g. the `(SELECT * FROM t1)` in `SELECT * FROM (SELECT * FROM t1)`.
 */
class BoundSubqueryRef : public BoundTableRef {
 public:
  /**
   * @brief Construct a bound subquery.
   *
   * The constructor and destructor are deliberately out of line. `SelectStatement`
   * is only forward-declared here — it cannot be included, because select_statement.h
   * needs `CTEList`, which is defined below in terms of this very class. Defining
   * either of these inline would instantiate `std::unique_ptr<SelectStatement>`'s
   * destructor against the incomplete type, which fails in any translation unit that
   * reaches this header without also having included select_statement.h.
   *
   * @param subquery The bound SELECT.
   * @param select_list_name The name of each item of the subquery's select list.
   * @param alias The name the subquery is known by in the outer scope.
   */
  explicit BoundSubqueryRef(std::unique_ptr<SelectStatement> subquery,
                            std::vector<std::vector<std::string>> select_list_name, std::string alias);

  ~BoundSubqueryRef() override;

  auto ToString() const -> std::string override;

  /** The bound SELECT. */
  std::unique_ptr<SelectStatement> subquery_;

  /** The name of each item of the subquery's select list. */
  std::vector<std::vector<std::string>> select_list_name_;

  /** The name the subquery is known by in the outer scope. */
  std::string alias_;
};

/** The CTEs a SELECT was declared with. */
using CTEList = std::vector<std::unique_ptr<BoundSubqueryRef>>;

}  // namespace bumblebee
