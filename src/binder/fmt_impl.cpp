//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// fmt_impl.cpp
//
// Identification: src/binder/fmt_impl.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// The ToString() implementations that cannot live in their headers, because they
// format a container of a type that is only complete once every binder header has
// been seen.
//
//===----------------------------------------------------------------------===//

#include <string>
#include <vector>

#include "binder/bound_expression.h"
#include "binder/expressions/bound_agg_call.h"
#include "binder/expressions/bound_func_call.h"
#include "binder/statement/select_statement.h"
#include "binder/table_ref/bound_cte_ref.h"
#include "binder/table_ref/bound_expression_list_ref.h"
#include "binder/table_ref/bound_subquery_ref.h"
#include "common/util/string_util.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

namespace bumblebee {

auto BoundFuncCall::ToString() const -> std::string { return fmt::format("{}({})", func_name_, args_); }

auto BoundAggCall::ToString() const -> std::string {
  if (is_distinct_) {
    return fmt::format("{}_distinct({})", func_name_, args_);
  }
  return fmt::format("{}({})", func_name_, args_);
}

auto BoundExpressionListRef::ToString() const -> std::string {
  return fmt::format("BoundExpressionListRef {{ identifier={}, values={} }}", identifier_, values_);
}

auto BoundCTERef::ToString() const -> std::string {
  return fmt::format("BoundCTERef {{ alias={}, cte={} }}", alias_, cte_name_);
}

// Out of line because BoundSubqueryRef only forward-declares SelectStatement; see
// the note on the constructor in bound_subquery_ref.h. This file has the complete
// type, so std::unique_ptr<SelectStatement>'s destructor can be instantiated here.
BoundSubqueryRef::BoundSubqueryRef(std::unique_ptr<SelectStatement> subquery,
                                   std::vector<std::vector<std::string>> select_list_name, std::string alias)
    : BoundTableRef(TableReferenceType::SUBQUERY),
      subquery_(std::move(subquery)),
      select_list_name_(std::move(select_list_name)),
      alias_(std::move(alias)) {}

BoundSubqueryRef::~BoundSubqueryRef() = default;

auto BoundSubqueryRef::ToString() const -> std::string {
  std::vector<std::string> columns;
  columns.reserve(select_list_name_.size());
  for (const auto &name : select_list_name_) {
    columns.push_back(fmt::format("{}", fmt::join(name, ".")));
  }
  return fmt::format("BoundSubqueryRef {{\n  alias={},\n  subquery={},\n  columns={},\n}}", alias_,
                     StringUtil::IndentAllLines(subquery_->ToString(), 2, true), columns);
}

}  // namespace bumblebee
