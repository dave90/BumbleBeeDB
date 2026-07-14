//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// keyword_helper.h
//
// Identification: src/include/binder/keyword_helper.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "common/config.h"

namespace bumblebee {

/** Helpers for deciding whether an identifier collides with a SQL keyword. */
class KeywordHelper {
 public:
  /**
   * @brief Is `text` a keyword of the SQL grammar?
   *
   * @param text The candidate identifier.
   * @return bool True if it is a keyword.
   */
  static auto IsKeyword(const std::string &text) -> bool;

  /**
   * @brief Does `text` have to be quoted to be written as an identifier?
   *
   * @param text The candidate identifier.
   * @return bool True if quoting is required.
   */
  static auto RequiresQuotes(const std::string &text) -> bool;

  /**
   * @brief Render `text` as an identifier, quoting and escaping it if it needs it.
   *
   * @param text The identifier.
   * @param quote The quote character.
   * @return std::string The rendered identifier.
   */
  static auto WriteOptionallyQuoted(const std::string &text, char quote = '"') -> std::string;
};

}  // namespace bumblebee
