//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// simplified_token.h
//
// Identification: src/include/binder/simplified_token.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace bumblebee {

/**
 * A simplified (dense) representation of the lexer's token types, used for
 * simple syntax highlighting.
 */
enum class SimplifiedTokenType : uint8_t {
  SIMPLIFIED_TOKEN_IDENTIFIER,
  SIMPLIFIED_TOKEN_NUMERIC_CONSTANT,
  SIMPLIFIED_TOKEN_STRING_CONSTANT,
  SIMPLIFIED_TOKEN_OPERATOR,
  SIMPLIFIED_TOKEN_KEYWORD,
  SIMPLIFIED_TOKEN_COMMENT
};

/** One token of the query text, with the offset it starts at. */
struct SimplifiedToken {
  /** The token type. */
  SimplifiedTokenType type_;
  /** The offset in the query text this token starts at. */
  int32_t start_;
};

/** How reserved a keyword is in the Postgres grammar. */
enum class KeywordCategory : uint8_t { KEYWORD_RESERVED, KEYWORD_UNRESERVED, KEYWORD_TYPE_FUNC, KEYWORD_COL_NAME };

/** One keyword of the Postgres grammar. */
struct ParserKeyword {
  /** The keyword text. */
  std::string name_;
  /** The keyword category. */
  KeywordCategory category_;
};

}  // namespace bumblebee
