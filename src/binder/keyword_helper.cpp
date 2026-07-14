//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// keyword_helper.cpp
//
// Identification: src/binder/keyword_helper.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// Derived from DuckDB, which is licensed under the MIT License.
// Copyright 2018-2022 Stichting DuckDB Foundation.
//
//===----------------------------------------------------------------------===//

#include "binder/keyword_helper.h"

#include <string>

#include "binder/binder.h"
#include "common/util/string_util.h"

namespace bumblebee {

auto KeywordHelper::IsKeyword(const std::string &text) -> bool { return Binder::IsKeyword(text); }

auto KeywordHelper::RequiresQuotes(const std::string &text) -> bool {
  for (size_t i = 0; i < text.size(); i++) {
    if (i > 0 && (text[i] >= '0' && text[i] <= '9')) {
      continue;
    }
    if (text[i] >= 'a' && text[i] <= 'z') {
      continue;
    }
    if (text[i] == '_') {
      continue;
    }
    return true;
  }
  return IsKeyword(text);
}

auto KeywordHelper::WriteOptionallyQuoted(const std::string &text, char quote) -> std::string {
  if (!RequiresQuotes(text)) {
    return text;
  }
  return std::string(1, quote) + StringUtil::Replace(text, std::string(1, quote), std::string(2, quote)) +
         std::string(1, quote);
}

}  // namespace bumblebee
