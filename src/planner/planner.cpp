//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// planner.cpp
//
// Identification: src/planner/planner.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "planner/planner.h"

#include <memory>
#include <utility>

#include "binder/bound_expression.h"
#include "binder/bound_statement.h"
#include "binder/statement/delete_statement.h"
#include "binder/statement/insert_statement.h"
#include "binder/statement/select_statement.h"
#include "binder/statement/update_statement.h"
#include "common/enums/statement_type.h"
#include "common/exception.h"
#include "fmt/format.h"

namespace bumblebee {

void Planner::PlanQuery(const BoundStatement &statement) {
  switch (statement.type_) {
    case StatementType::SELECT_STATEMENT: {
      plan_ = PlanSelect(dynamic_cast<const SelectStatement &>(statement));
      return;
    }
    case StatementType::INSERT_STATEMENT: {
      plan_ = PlanInsert(dynamic_cast<const InsertStatement &>(statement));
      return;
    }
    case StatementType::DELETE_STATEMENT: {
      plan_ = PlanDelete(dynamic_cast<const DeleteStatement &>(statement));
      return;
    }
    case StatementType::UPDATE_STATEMENT: {
      plan_ = PlanUpdate(dynamic_cast<const UpdateStatement &>(statement));
      return;
    }
    default:
      throw PlannerException(fmt::format("the {} statement cannot be planned", statement.type_));
  }
}

auto Planner::MakeOutputSchema(const std::vector<std::pair<std::string, LogicalType>> &exprs) -> SchemaRef {
  std::vector<Column> cols;
  cols.reserve(exprs.size());
  for (const auto &[col_name, type] : exprs) {
    cols.emplace_back(Column::Make(col_name, type));
  }
  return std::make_shared<Schema>(cols);
}

void PlannerContext::AddAggregation(std::unique_ptr<BoundExpression> expr) {
  if (!allow_aggregation_) {
    throw PlannerException("an aggregate call is not allowed in this position");
  }
  aggregations_.push_back(std::move(expr));
}

}  // namespace bumblebee
