//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// binder.h
//
// Identification: src/include/binder/binder.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "binder/simplified_token.h"
#include "binder/statement/select_statement.h"
#include "binder/tokens.h"
#include "catalog/catalog.h"
#include "catalog/column.h"
#include "common/macros.h"
#include "nodes/parsenodes.hpp"
#include "nodes/pg_list.hpp"
#include "pg_definitions.hpp"
#include "postgres_parser.hpp"
#include "type/logical_type.h"

namespace duckdb_libpgquery {
struct PGList;
struct PGSelectStmt;
struct PGAConst;
struct PGAStar;
struct PGFuncCall;
struct PGNode;
struct PGColumnRef;
struct PGResTarget;
struct PGAExpr;
struct PGJoinExpr;
}  // namespace duckdb_libpgquery

namespace bumblebee {

class Catalog;
class BoundColumnRef;
class BoundExpression;
class BoundTableRef;
class BoundBaseTableRef;
class BoundExpressionListRef;
class BoundOrderBy;
class BoundSubqueryRef;
class CreateStatement;
class DropStatement;
class TransactionStatement;
class ExplainStatement;
class DeleteStatement;
class UpdateStatement;
class InsertStatement;

/**
 * The binder turns the Postgres parse tree into a tree of Bound* nodes: every
 * table resolved against the catalog, every column reference resolved against the
 * tables in scope, every clause unambiguous. It is the last stage that knows
 * anything about SQL syntax, and the first that knows anything about the catalog.
 */
class Binder {
 public:
  /**
   * @brief Construct a binder over a catalog.
   *
   * @param catalog The catalog. It MUST outlive the binder — the binder holds a reference.
   */
  explicit Binder(const Catalog &catalog);

  /**
   * @brief Parse a query and save its statements in `statement_nodes_`.
   *
   * @param query The SQL text.
   */
  void ParseAndSave(const std::string &query);

  /** @brief Is `text` a keyword of the SQL grammar? */
  static auto IsKeyword(const std::string &text) -> bool;

  /** @return Every keyword of the SQL grammar. */
  static auto KeywordList() -> std::vector<ParserKeyword>;

  /**
   * @brief Tokenize a query, for syntax highlighting.
   *
   * @param query The SQL text.
   * @return std::vector<SimplifiedToken> The tokens and their offsets.
   */
  static auto Tokenize(const std::string &query) -> std::vector<SimplifiedToken>;

  /** @brief Save every statement of a parse tree into `statement_nodes_`. */
  void SaveParseTree(duckdb_libpgquery::PGList *tree);

  /**
   * @brief Bind one statement of the parse tree.
   *
   * @param stmt The parse-tree node.
   * @return std::unique_ptr<BoundStatement> The bound statement.
   */
  auto BindStatement(duckdb_libpgquery::PGNode *stmt) -> std::unique_ptr<BoundStatement>;

  /** @brief Render a Postgres node tag as a string, for error messages. */
  static auto NodeTagToString(duckdb_libpgquery::PGNodeTag type) -> std::string;

  // The following parts are undocumented. One `BindXXX` function simply corresponds to a
  // node type in the Postgres parse tree.

  auto BindExplain(duckdb_libpgquery::PGExplainStmt *stmt) -> std::unique_ptr<ExplainStatement>;

  auto BindCreate(duckdb_libpgquery::PGCreateStmt *pg_stmt) -> std::unique_ptr<CreateStatement>;

  auto BindDrop(duckdb_libpgquery::PGDropStmt *stmt) -> std::unique_ptr<DropStatement>;

  auto BindTransaction(duckdb_libpgquery::PGTransactionStmt *stmt) -> std::unique_ptr<TransactionStatement>;

  auto BindColumnDefinition(duckdb_libpgquery::PGColumnDef *cdef) -> Column;

  auto BindSelect(duckdb_libpgquery::PGSelectStmt *pg_stmt) -> std::unique_ptr<SelectStatement>;

  auto BindRangeSubselect(duckdb_libpgquery::PGRangeSubselect *root) -> std::unique_ptr<BoundTableRef>;

  auto BindSubquery(duckdb_libpgquery::PGSelectStmt *node, const std::string &alias)
      -> std::unique_ptr<BoundSubqueryRef>;

  auto BindSelectList(duckdb_libpgquery::PGList *list) -> std::vector<std::unique_ptr<BoundExpression>>;

  auto BindWhere(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression>;

  auto BindGroupBy(duckdb_libpgquery::PGList *list, const std::vector<std::unique_ptr<BoundExpression>> &select_list)
      -> std::vector<std::unique_ptr<BoundExpression>>;
  auto ResolveGroupByAlias(const std::vector<std::unique_ptr<BoundExpression>> &select_list,
                           duckdb_libpgquery::PGNode *node) -> std::unique_ptr<BoundExpression>;

  auto BindHaving(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression>;

  auto BindExpression(duckdb_libpgquery::PGNode *node) -> std::unique_ptr<BoundExpression>;

  auto BindExpressionList(duckdb_libpgquery::PGList *list) -> std::vector<std::unique_ptr<BoundExpression>>;

  auto BindConstant(duckdb_libpgquery::PGAConst *node) -> std::unique_ptr<BoundExpression>;

  auto BindColumnRef(duckdb_libpgquery::PGColumnRef *node) -> std::unique_ptr<BoundExpression>;

  auto BindResTarget(duckdb_libpgquery::PGResTarget *root) -> std::unique_ptr<BoundExpression>;

  auto BindStar(duckdb_libpgquery::PGAStar *node) -> std::unique_ptr<BoundExpression>;

  auto BindFuncCall(duckdb_libpgquery::PGFuncCall *root) -> std::unique_ptr<BoundExpression>;

  auto BindAExpr(duckdb_libpgquery::PGAExpr *root) -> std::unique_ptr<BoundExpression>;
  auto BindSubLink(duckdb_libpgquery::PGSubLink *root) -> std::unique_ptr<BoundExpression>;
  auto ResolveOuterColumn(const std::vector<std::string> &col_name) -> std::unique_ptr<BoundExpression>;
  auto BindTypeCast(duckdb_libpgquery::PGTypeCast *root) -> std::unique_ptr<BoundExpression>;
  auto ResolveTypeName(duckdb_libpgquery::PGTypeName *type_name) -> LogicalType;

  auto BindBoolExpr(duckdb_libpgquery::PGBoolExpr *root) -> std::unique_ptr<BoundExpression>;

  auto BindFrom(duckdb_libpgquery::PGList *list) -> std::unique_ptr<BoundTableRef>;

  auto BindBaseTableRef(std::string table_name, std::optional<std::string> alias) -> std::unique_ptr<BoundBaseTableRef>;

  auto BindRangeVar(duckdb_libpgquery::PGRangeVar *table_ref) -> std::unique_ptr<BoundTableRef>;

  auto BindTableRef(duckdb_libpgquery::PGNode *node) -> std::unique_ptr<BoundTableRef>;

  auto BindJoin(duckdb_libpgquery::PGJoinExpr *root) -> std::unique_ptr<BoundTableRef>;

  auto GetAllColumns(const BoundTableRef &scope) -> std::vector<std::unique_ptr<BoundExpression>>;

  auto ResolveColumn(const BoundTableRef &scope, const std::vector<std::string> &col_name)
      -> std::unique_ptr<BoundExpression>;

  auto ResolveColumnInternal(const BoundTableRef &table_ref, const std::vector<std::string> &col_name)
      -> std::unique_ptr<BoundExpression>;

  auto ResolveColumnRefFromSelectList(const std::vector<std::vector<std::string>> &subquery_select_list,
                                      const std::vector<std::string> &col_name) -> std::unique_ptr<BoundColumnRef>;

  auto ResolveColumnRefFromBaseTableRef(const BoundBaseTableRef &table_ref, const std::vector<std::string> &col_name)
      -> std::unique_ptr<BoundColumnRef>;

  auto ResolveColumnRefFromSubqueryRef(const BoundSubqueryRef &subquery_ref, const std::string &alias,
                                       const std::vector<std::string> &col_name) -> std::unique_ptr<BoundColumnRef>;

  auto BindInsert(duckdb_libpgquery::PGInsertStmt *pg_stmt) -> std::unique_ptr<InsertStatement>;

  auto BindValuesList(duckdb_libpgquery::PGList *list) -> std::unique_ptr<BoundExpressionListRef>;

  auto BindLimitCount(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression>;

  auto BindLimitOffset(duckdb_libpgquery::PGNode *root) -> std::unique_ptr<BoundExpression>;

  auto BindSort(duckdb_libpgquery::PGList *list,
                const std::vector<std::unique_ptr<BoundExpression>> &select_list)
      -> std::vector<std::unique_ptr<BoundOrderBy>>;

  auto ResolveOrderByFromSelectList(const std::vector<std::unique_ptr<BoundExpression>> &select_list,
                                    const std::string &name) -> std::unique_ptr<BoundExpression>;

  auto BindDelete(duckdb_libpgquery::PGDeleteStmt *stmt) -> std::unique_ptr<DeleteStatement>;

  auto BindUpdate(duckdb_libpgquery::PGUpdateStmt *stmt) -> std::unique_ptr<UpdateStatement>;

  auto BindCTE(duckdb_libpgquery::PGWithClause *node) -> std::vector<std::unique_ptr<BoundSubqueryRef>>;

  /**
   * Saves and restores the binder's scope. Any function that modifies the scope MUST
   * hold one of these, so that the scope is recovered when the function returns.
   */
  class ContextGuard {
   public:
    ContextGuard(const BoundTableRef **scope, const CTEList **cte_scope) {
      old_scope_ = *scope;
      scope_ptr_ = scope;
      old_cte_scope_ = *cte_scope;
      cte_scope_ptr_ = cte_scope;
      *scope = nullptr;
      // Do not reset the CTE scope: we want to keep the ones inherited from the parent.
    }

    ~ContextGuard() {
      *scope_ptr_ = old_scope_;
      *cte_scope_ptr_ = old_cte_scope_;
    }

    ContextGuard(const ContextGuard &) = delete;
    auto operator=(const ContextGuard &) -> ContextGuard & = delete;
    ContextGuard(ContextGuard &&) = delete;
    auto operator=(ContextGuard &&) -> ContextGuard & = delete;

   private:
    const BoundTableRef *old_scope_;
    const BoundTableRef **scope_ptr_;
    const CTEList *old_cte_scope_;
    const CTEList **cte_scope_ptr_;
  };

  /** @return A guard that restores the current scope when it goes out of scope. */
  auto NewContext() -> ContextGuard { return ContextGuard(&scope_, &cte_scope_); }

  /**
   * Opens an outer-scope level for an expression subquery (SubLink): the enclosing query's FROM
   * scope becomes visible to the subquery as a correlation fallback (a normal in-scope column
   * always wins). Popped when the guard dies. Only BindSubLink holds one — derived tables in a
   * FROM clause deliberately stay uncorrelated.
   */
  class OuterScopeGuard {
   public:
    explicit OuterScopeGuard(std::vector<const BoundTableRef *> *stack, const BoundTableRef *scope)
        : stack_(stack) {
      stack_->push_back(scope);
    }
    ~OuterScopeGuard() { stack_->pop_back(); }

    OuterScopeGuard(const OuterScopeGuard &) = delete;
    auto operator=(const OuterScopeGuard &) -> OuterScopeGuard & = delete;
    OuterScopeGuard(OuterScopeGuard &&) = delete;
    auto operator=(OuterScopeGuard &&) -> OuterScopeGuard & = delete;

   private:
    std::vector<const BoundTableRef *> *stack_;
  };

  /** One parse-tree node per statement of the last parsed query. */
  std::vector<duckdb_libpgquery::PGNode *> statement_nodes_;

 private:
  /** The catalog to bind against. Callers MUST keep it alive for as long as the binder. */
  const Catalog &catalog_;

  /** The tables currently in scope, used to resolve column references. */
  const BoundTableRef *scope_{nullptr};

  /** The CTEs currently in scope, used to resolve table references. */
  const CTEList *cte_scope_{nullptr};

  /**
   * The enclosing queries' FROM scopes, one per SubLink nesting level (outermost first; entries may
   * be null for an enclosing SELECT without FROM). A column that fails normal resolution falls back
   * to these, innermost first, and binds as a BoundOuterColumnRef.
   */
  std::vector<const BoundTableRef *> outer_scopes_;

  /** How many outer (correlated) column refs have been bound; BindSubLink diffs it around the
   * subquery bind to know whether the subquery is correlated. */
  size_t outer_refs_bound_{0};

  /** Supplies unique names for the items that do not have one (subqueries, VALUES clauses). */
  size_t universal_id_{0};

  duckdb::PostgresParser parser_;
};

}  // namespace bumblebee
