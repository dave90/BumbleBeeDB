//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_util.h
//
// Identification: src/include/common/util/string_util.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

namespace bumblebee {

/**
 * StringUtil provides INEFFICIENT utility functions for working with strings. They should only be used for debugging.
 */
class StringUtil {
 public:
  static auto Contains(const std::string &haystack, const std::string &needle) -> bool;

  static auto ContainsAfter(const std::string &keyword, const std::string &haystack, const std::string &needle) -> bool;

  static auto StartsWith(const std::string &str, const std::string &prefix) -> bool;

  static auto EndsWith(const std::string &str, const std::string &suffix) -> bool;

  static auto Repeat(const std::string &str, std::size_t n) -> std::string;

  static auto Split(const std::string &str, char delimiter) -> std::vector<std::string>;

  static auto Join(const std::vector<std::string> &input, const std::string &separator) -> std::string;

  /** @return join multiple items of container with given size, transformed to string
  using function, into one string using the given separator. */
  template <typename C, typename S, typename Func>
  static auto Join(const C &input, S count, const std::string &separator, Func f) -> std::string {
    // The result
    std::string result;

    // If the input isn't empty, append the first element. We do this so we
    // don't need to introduce an if into the loop.
    if (count > 0) {
      result += f(input[0]);
    }

    // Append the remaining input components, after the first
    for (size_t i = 1; i < count; i++) {
      result += separator + f(input[i]);
    }

    return result;
  }

  static auto Prefix(const std::string &str, const std::string &prefix) -> std::string;

  static auto FormatSize(uint64_t bytes) -> std::string;

  static auto Bold(const std::string &str) -> std::string;

  static auto Upper(const std::string &str) -> std::string;

  static auto Lower(const std::string &str) -> std::string;

  static auto Format(std::string fmt_str, ...) -> std::string;

  static auto Split(const std::string &input, const std::string &split) -> std::vector<std::string>;

  static auto Count(const std::string &input, const std::string &str) -> size_t;

  static void RTrim(std::string *str);

  static void LTrim(std::string *str);

  static auto Indent(int num_indent) -> std::string;

  static auto Strip(const std::string &str, char c) -> std::string;

  static auto Replace(std::string source, const std::string &from, const std::string &to) -> std::string;

  static auto IndentAllLines(const std::string &lines, size_t num_indent, bool except_first_line = false)
      -> std::string;

  // -- Per-character classification -----------------------------------------
  // These are called once per byte by the date/timestamp parsers, so they are defined
  // inline: a call site in another translation unit must not pay a call per character.

  /** @return True if `c` is an ASCII digit. */
  static auto CharacterIsDigit(char c) -> bool { return c >= '0' && c <= '9'; }

  /** @return True if `c` is any ASCII whitespace. */
  static auto CharacterIsSpace(char c) -> bool {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
  }

  /** @return `c` lowercased, if it is an ASCII upper-case letter. */
  static auto CharacterToLower(char c) -> char {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<char>(c - ('A' - 'a'));
    }
    return c;
  }
};

}  // namespace bumblebee
