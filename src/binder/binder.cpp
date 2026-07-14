//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// binder.cpp
//
// Identification: src/binder/binder.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// Portions of the binder are derived from DuckDB, which is licensed under the
// MIT License. Copyright 2018-2022 Stichting DuckDB Foundation.
//
//===----------------------------------------------------------------------===//

#include "binder/binder.h"

#include <string>
#include <vector>

#include "binder/bound_statement.h"
#include "common/exception.h"
#include "fmt/format.h"
#include "pg_definitions.hpp"
#include "postgres_parser.hpp"

namespace bumblebee {

Binder::Binder(const Catalog &catalog) : catalog_(catalog) {}

void Binder::ParseAndSave(const std::string &query) {
  parser_.Parse(query);
  if (!parser_.success) {
    throw ParserException(fmt::format("Query failed to parse: {}", parser_.error_message));
  }

  if (parser_.parse_tree == nullptr) {
    // An empty statement, e.g. a query that was only whitespace. Nothing to save.
    return;
  }

  SaveParseTree(parser_.parse_tree);
}

auto Binder::IsKeyword(const std::string &text) -> bool { return duckdb::PostgresParser::IsKeyword(text); }

auto Binder::KeywordList() -> std::vector<ParserKeyword> {
  auto keywords = duckdb::PostgresParser::KeywordList();
  std::vector<ParserKeyword> result;
  result.reserve(keywords.size());
  for (auto &kw : keywords) {
    ParserKeyword res;
    res.name_ = kw.text;
    switch (kw.category) {
      case duckdb_libpgquery::PGKeywordCategory::PG_KEYWORD_RESERVED:
        res.category_ = KeywordCategory::KEYWORD_RESERVED;
        break;
      case duckdb_libpgquery::PGKeywordCategory::PG_KEYWORD_UNRESERVED:
        res.category_ = KeywordCategory::KEYWORD_UNRESERVED;
        break;
      case duckdb_libpgquery::PGKeywordCategory::PG_KEYWORD_TYPE_FUNC:
        res.category_ = KeywordCategory::KEYWORD_TYPE_FUNC;
        break;
      case duckdb_libpgquery::PGKeywordCategory::PG_KEYWORD_COL_NAME:
        res.category_ = KeywordCategory::KEYWORD_COL_NAME;
        break;
      default:
        throw ParserException("Unrecognized keyword category");
    }
    result.push_back(res);
  }
  return result;
}

auto Binder::Tokenize(const std::string &query) -> std::vector<SimplifiedToken> {
  auto pg_tokens = duckdb::PostgresParser::Tokenize(query);
  std::vector<SimplifiedToken> result;
  result.reserve(pg_tokens.size());
  for (auto &pg_token : pg_tokens) {
    SimplifiedToken token;
    switch (pg_token.type) {
      case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_IDENTIFIER:
        token.type_ = SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
        break;
      case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
        token.type_ = SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT;
        break;
      case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_STRING_CONSTANT:
        token.type_ = SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT;
        break;
      case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_OPERATOR:
        token.type_ = SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR;
        break;
      case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_KEYWORD:
        token.type_ = SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD;
        break;
      case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_COMMENT:
        token.type_ = SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT;
        break;
      default:
        throw ParserException("Unrecognized token category");
    }
    token.start_ = pg_token.start;
    result.push_back(token);
  }
  return result;
}

}  // namespace bumblebee
