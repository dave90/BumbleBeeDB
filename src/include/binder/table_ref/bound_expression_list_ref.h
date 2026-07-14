//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_expression_list_ref.h
//
// Identification: src/include/binder/table_ref/bound_expression_list_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "binder/bound_table_ref.h"
#include "fmt/format.h"

namespace bumblebee {

class BoundExpression;

/**
 * A VALUES clause, e.g. the `VALUES (1, 2), (3, 4)` in an INSERT.
 */
class BoundExpressionListRef : public BoundTableRef {
 public:
  /**
   * @brief Construct a bound VALUES clause.
   *
   * @param values One vector of expressions per row.
   * @param identifier A unique name for this VALUES clause, so that column references into it resolve.
   */
  explicit BoundExpressionListRef(std::vector<std::vector<std::unique_ptr<BoundExpression>>> values,
                                  std::string identifier)
      : BoundTableRef(TableReferenceType::EXPRESSION_LIST),
        values_(std::move(values)),
        identifier_(std::move(identifier)) {}

  auto ToString() const -> std::string override;

  /** One vector of expressions per row. */
  std::vector<std::vector<std::unique_ptr<BoundExpression>>> values_;

  /** A unique name for this VALUES clause. */
  std::string identifier_;
};

}  // namespace bumblebee
