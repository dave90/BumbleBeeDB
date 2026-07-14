//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// frontend_test_util.h
//
// Identification: test/unit/include/frontend_test_util.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_statement.h"
#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "execution/plans/abstract_plan.h"
#include "type/logical_type.h"

namespace bumblebee {

/**
 * @brief The fixed catalog every frontend test binds against.
 *
 * - `y (x INT, z INT, a INT, b INT, c INT)`
 * - `a (x INT, y INT)`
 * - `b (x INT, y INT)`
 * - `c (x VARCHAR(100), y VARCHAR(100))`
 * - `t1_1k (v1 INT, v2 INT)`      -- the _1k / _10k suffixes feed
 * - `t2_10k (v3 INT, v4 INT)`     -- Optimizer::EstimatedCardinality
 * - `arr (id INT, tags INT[], fixed INT[3])`
 *
 * @return std::unique_ptr<Catalog> The populated catalog.
 */
inline auto MakeTestCatalog() -> std::unique_ptr<Catalog> {
  auto catalog = std::make_unique<Catalog>();
  const auto kInt = LogicalType(LogicalTypeId::INTEGER);

  catalog->CreateTable("y", Schema(std::vector{Column{"x", kInt}, Column{"z", kInt}, Column{"a", kInt},
                                               Column{"b", kInt}, Column{"c", kInt}}));
  catalog->CreateTable("a", Schema(std::vector{Column{"x", kInt}, Column{"y", kInt}}));
  catalog->CreateTable("b", Schema(std::vector{Column{"x", kInt}, Column{"y", kInt}}));
  catalog->CreateTable("c", Schema(std::vector{Column{"x", LogicalType(LogicalTypeId::STRING), 100},
                                               Column{"y", LogicalType(LogicalTypeId::STRING), 100}}));
  catalog->CreateTable("t1_1k", Schema(std::vector{Column{"v1", kInt}, Column{"v2", kInt}}));
  catalog->CreateTable("t2_10k", Schema(std::vector{Column{"v3", kInt}, Column{"v4", kInt}}));
  catalog->CreateTable(
      "arr", Schema(std::vector{Column{"id", kInt},
                                Column{"tags", LogicalType::List(kInt), 0},
                                Column{"fixed", LogicalType::Array(kInt, 3), 0}}));
  return catalog;
}

/**
 * @brief Parse and bind a query against a catalog.
 *
 * @param catalog The catalog to bind against.
 * @param query The SQL text. May contain more than one statement.
 * @return std::vector<std::unique_ptr<BoundStatement>> One bound statement per statement in `query`.
 */
inline auto TryBind(const Catalog &catalog, const std::string &query)
    -> std::vector<std::unique_ptr<BoundStatement>> {
  Binder binder(catalog);
  binder.ParseAndSave(query);

  std::vector<std::unique_ptr<BoundStatement>> statements;
  statements.reserve(binder.statement_nodes_.size());
  for (auto *stmt : binder.statement_nodes_) {
    statements.emplace_back(binder.BindStatement(stmt));
  }
  return statements;
}

/**
 * @brief Count the nodes of a given type anywhere in a plan tree.
 *
 * @param plan The plan tree.
 * @param type The node type to count.
 * @return size_t How many there are.
 */
inline auto CountPlanNodes(const AbstractPlanNodeRef &plan, PlanType type) -> size_t {
  size_t count = plan->GetType() == type ? 1 : 0;
  for (const auto &child : plan->GetChildren()) {
    count += CountPlanNodes(child, type);
  }
  return count;
}

/**
 * @brief Find the first node of a given type in a plan tree, pre-order.
 *
 * @param plan The plan tree.
 * @param type The node type to look for.
 * @return AbstractPlanNodeRef The node, or nullptr if there is none.
 */
inline auto FindPlanNode(const AbstractPlanNodeRef &plan, PlanType type) -> AbstractPlanNodeRef {
  if (plan->GetType() == type) {
    return plan;
  }
  for (const auto &child : plan->GetChildren()) {
    if (auto found = FindPlanNode(child, type); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace bumblebee
