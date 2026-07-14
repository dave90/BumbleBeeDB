//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bound_cte_ref.h
//
// Identification: src/include/binder/table_ref/bound_cte_ref.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>

#include "binder/bound_table_ref.h"
#include "fmt/format.h"

namespace bumblebee {

/**
 * A reference to a CTE, e.g. the `x` in `WITH x AS (SELECT 1) SELECT * FROM x`.
 */
class BoundCTERef : public BoundTableRef {
 public:
  /**
   * @brief Construct a reference to a CTE.
   *
   * @param cte_name The name of the CTE being referenced.
   * @param alias The name the reference is known by in the current scope.
   */
  explicit BoundCTERef(std::string cte_name, std::string alias)
      : BoundTableRef(TableReferenceType::CTE), cte_name_(std::move(cte_name)), alias_(std::move(alias)) {}

  auto ToString() const -> std::string override;

  /** The name of the CTE being referenced. */
  std::string cte_name_;

  /** The name the reference is known by in the current scope. */
  std::string alias_;
};

}  // namespace bumblebee
