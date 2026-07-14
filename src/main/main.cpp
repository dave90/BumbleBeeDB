//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// main.cpp
//
// Identification: src/main/main.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "bumblebee_instance.h"
#include "common/exception.h"
#include "common/util/string_util.h"

namespace {

/** @brief Run one statement and print the result, keeping the shell alive on error. */
auto RunStatement(bumblebee::BumbleBeeInstance &instance, const std::string &sql) -> bool {
  try {
    bumblebee::SimpleStreamWriter writer(std::cout);
    instance.ExecuteSql(sql, writer);
    return true;
  } catch (const bumblebee::Exception &e) {
    std::cerr << e.what() << std::endl;
    return false;
  }
}

}  // namespace

auto main(int argc, char **argv) -> int {
  // Exception's constructor traces to stderr in debug builds, which is useful when a
  // test blows up but is just noise here: the shell reports the error itself.
  bumblebee::global_disable_exception_print.store(true);

  auto instance = std::make_unique<bumblebee::BumbleBeeInstance>();
  instance->GenerateMockTable();

  // `-c "<sql>"` runs one statement and exits, which makes the shell scriptable.
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      return RunStatement(*instance, argv[i + 1]) ? 0 : 1;
    }
  }

  std::cout << "BumbleBeeDB — type \\help for help, Ctrl-D to exit." << std::endl;

  std::string query;
  while (true) {
    std::cout << (query.empty() ? "bumblebee> " : "        ... ") << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << std::endl;
      break;
    }

    // A meta-command is a whole statement on its own; anything else accumulates
    // until a line ends in a semicolon, so a statement can span lines.
    if (query.empty() && !line.empty() && line[0] == '\\') {
      RunStatement(*instance, line);
      continue;
    }

    if (!query.empty()) {
      query += " ";
    }
    query += line;

    auto trimmed = query;
    bumblebee::StringUtil::RTrim(&trimmed);
    if (trimmed.empty()) {
      query.clear();
      continue;
    }
    if (trimmed.back() != ';') {
      continue;
    }

    RunStatement(*instance, trimmed);
    query.clear();
  }

  return 0;
}
