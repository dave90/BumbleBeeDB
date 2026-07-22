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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "bumblebee_instance.h"
#include "common/exception.h"
#include "common/util/string_util.h"
#include "linenoise.h"

namespace {

/**
 * The ASCII record-separator (0x1e) that prefixes a per-statement status line in `--test-protocol`
 * mode. It cannot appear in SQL result text, so the e2e harness can unambiguously tell a status
 * line ("<RS>ok" / "<RS>err <message>") apart from a result row.
 */
constexpr char kStatusMarker = '\x1e';

/**
 * @brief Run one statement and print the result, keeping the shell alive on error.
 *
 * In `--test-protocol` mode the result is written header-free and space-separated (so a query's
 * rows compare directly against a `.slt` file's expected block), and a status line is emitted to
 * stdout after the statement: `<RS>ok` on success, `<RS>err <message>` on failure. Otherwise the
 * human shell format (tab-separated with a header) is used and errors go to stderr.
 */
auto RunStatement(bumblebee::BumbleBeeInstance &instance, const std::string &sql, bool test_protocol,
                  bumblebee::idx_t max_rows) -> bool {
  try {
    if (test_protocol) {
      // The e2e harness must see every row, so it is never truncated (unlimited display).
      bumblebee::SimpleStreamWriter writer(std::cout, /*disable_header=*/true, /*separator=*/" ",
                                           /*max_display_rows=*/0);
      instance.ExecuteSql(sql, writer);
      std::cout << kStatusMarker << "ok" << std::endl;
    } else {
      bumblebee::SimpleStreamWriter writer(std::cout, /*disable_header=*/false, /*separator=*/"\t", max_rows);
      instance.ExecuteSql(sql, writer);
    }
    return true;
  } catch (const bumblebee::Exception &e) {
    if (test_protocol) {
      std::cout << kStatusMarker << "err " << e.what() << std::endl;
    } else {
      std::cerr << e.what() << std::endl;
    }
    return false;
  }
}

}  // namespace

auto main(int argc, char **argv) -> int {
  // Exception's constructor traces to stderr in debug builds, which is useful when a
  // test blows up but is just noise here: the shell reports the error itself.
  bumblebee::global_disable_exception_print.store(true);

  // By default the shell is durable, backed by `bb.db` — the catalog (and, once the execution engine
  // lands, the rows) survives across runs. `--db <path>` overrides the file; `--memory` (or `-m`) runs
  // a purely in-memory instance that persists nothing.
  std::filesystem::path db_path = "bb.db";
  bool in_memory = false;
  // `--test-protocol` puts the shell in machine-drivable mode for the e2e harness; `--no-seed` skips the
  // demo tables so a run starts from an empty catalog. The remaining flags override config.h defaults so a
  // test can exercise a statement under a tighter memory budget, the external operators, or a smaller pool.
  bool test_protocol = false;
  bool no_seed = false;
  bool prefer_external = false;
  bumblebee::idx_t max_memory = bumblebee::MAX_MEMORY;
  bumblebee::idx_t max_threads = 0;  // 0 = leave the hardware-detected default
  bumblebee::idx_t morsel_pages = bumblebee::MORSEL_PAGES;
  bumblebee::idx_t morsel_size = bumblebee::MORSEL_SIZE;
  size_t num_frames = bumblebee::BUFFER_POOL_SIZE;
  // How many result rows the interactive shell prints before truncating; 0 shows all. Keeps a giant
  // SELECT from flooding the terminal.
  bumblebee::idx_t max_rows = 100;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--memory") == 0 || std::strcmp(argv[i], "-m") == 0) {
      in_memory = true;
    } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
      db_path = argv[++i];
    } else if (std::strcmp(argv[i], "--test-protocol") == 0) {
      test_protocol = true;
    } else if (std::strcmp(argv[i], "--no-seed") == 0) {
      no_seed = true;
    } else if (std::strcmp(argv[i], "--prefer-external") == 0) {
      prefer_external = true;
    } else if (std::strcmp(argv[i], "--max-memory") == 0 && i + 1 < argc) {
      max_memory = static_cast<bumblebee::idx_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      max_threads = static_cast<bumblebee::idx_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--morsel-pages") == 0 && i + 1 < argc) {
      morsel_pages = static_cast<bumblebee::idx_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--morsel-size") == 0 && i + 1 < argc) {
      morsel_size = static_cast<bumblebee::idx_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      num_frames = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--max-rows") == 0 && i + 1 < argc) {
      max_rows = static_cast<bumblebee::idx_t>(std::strtoull(argv[++i], nullptr, 10));
    }
  }

  auto instance = in_memory ? std::make_unique<bumblebee::BumbleBeeInstance>()
                            : std::make_unique<bumblebee::BumbleBeeInstance>(db_path, num_frames);
  instance->prefer_external_ = prefer_external;
  instance->max_memory_ = max_memory;
  instance->max_threads_ = max_threads;
  instance->morsel_pages_ = morsel_pages;
  instance->morsel_size_ = morsel_size;
  // Seed the demo tables only when the catalog is empty and seeding is not suppressed — always for an
  // in-memory instance, and for a brand-new file so reopening an existing database does not fail on a
  // duplicate CREATE. The e2e harness passes `--no-seed` so each `.slt` file starts from a clean catalog.
  if (!no_seed && instance->catalog_->GetTableNames().empty()) {
    instance->GenerateMockTable();
  }

  // `-c "<sql>"` runs one statement and exits, which makes the shell scriptable.
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      return RunStatement(*instance, argv[i + 1], test_protocol, max_rows) ? 0 : 1;
    }
  }

  // The banner and persistent history are shell niceties; the e2e harness wants a clean, side-effect-free
  // stdout, so both are skipped in `--test-protocol` mode.
  if (!test_protocol) {
    std::cout << "BumbleBeeDB (" << (in_memory ? "in-memory" : db_path.string())
              << ") — type \\help for help, Ctrl-D to exit." << std::endl;
  }

  // linenoise gives the prompt arrow-key line editing and command history. History persists across
  // sessions in ~/.bumblebee_history (best-effort — a read-only HOME just means no saved history).
  std::string history_path;
  if (!test_protocol) {
    if (const char *home = std::getenv("HOME"); home != nullptr) {
      history_path = std::string(home) + "/.bumblebee_history";
      linenoiseHistoryLoad(history_path.c_str());
    }
    linenoiseHistorySetMaxLen(1000);
  }

  std::string query;
  while (true) {
    const char *prompt = query.empty() ? "bumblebee> " : "        ... ";
    char *raw = linenoise(prompt);
    if (raw == nullptr) {
      // linenoise returns null on Ctrl-C (errno EAGAIN) — cancel the in-progress statement — and on
      // Ctrl-D / EOF, which exits the shell.
      if (errno == EAGAIN) {
        query.clear();
        continue;
      }
      std::cout << std::endl;
      break;
    }
    std::string line(raw);
    linenoiseFree(raw);

    // Record each entered line so the up/down arrows recall it; persist so history survives a restart.
    if (!line.empty()) {
      linenoiseHistoryAdd(line.c_str());
      if (!history_path.empty()) {
        linenoiseHistorySave(history_path.c_str());
      }
    }

    // `\echo <text>` echoes its argument verbatim to stdout, bypassing the engine. The e2e harness
    // sends it after each record as a sentinel: it reads the record's output up to the echoed token,
    // so it can frame a record's results without knowing how many status lines precede them.
    static const std::string kEcho = "\\echo ";
    if (query.empty() && line.rfind(kEcho, 0) == 0) {
      std::cout << line.substr(kEcho.size()) << std::endl;
      continue;
    }

    // A meta-command is a whole statement on its own; anything else accumulates
    // until a line ends in a semicolon, so a statement can span lines.
    if (query.empty() && !line.empty() && line[0] == '\\') {
      RunStatement(*instance, line, test_protocol, max_rows);
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

    RunStatement(*instance, trimmed, test_protocol, max_rows);
    query.clear();
  }

  return 0;
}
