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
#include <chrono>  // NOLINT
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
static auto RunStatement(bumblebee::BumbleBeeInstance &instance, const std::string &sql, bool test_protocol,
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

/** @brief Everything about the shell that `argv` can configure. Defaults are the shell's defaults. */
struct ShellOptions {
  // By default the shell is durable, backed by `bb.db` — the catalog and rows survive across runs.
  // `--db <path>` overrides the file; `--memory` (or `-m`) runs a purely in-memory instance.
  std::filesystem::path db_path{"bb.db"};
  bool in_memory{false};
  // `--test-protocol` puts the shell in machine-drivable mode for the e2e harness; `--no-seed` skips
  // the demo tables so a run starts from an empty catalog. The rest override config.h defaults so a
  // test can exercise a statement under a tighter memory budget, the external operators, or a
  // smaller pool.
  bool test_protocol{false};
  bool no_seed{false};
  bool prefer_external{false};
  bumblebee::idx_t max_memory{bumblebee::MAX_MEMORY};
  bumblebee::idx_t max_threads{0};  // 0 = leave the hardware-detected default
  bumblebee::idx_t morsel_pages{bumblebee::MORSEL_PAGES};
  bumblebee::idx_t morsel_size{bumblebee::MORSEL_SIZE};
  size_t num_frames{bumblebee::BUFFER_POOL_SIZE};
  // How long a transaction may stay open before `\gc` aborts it. Runtime-configurable (unlike the
  // compile-time STANDARD_VECTOR_SIZE) so tests can shrink it to milliseconds via `--txn-timeout`.
  bumblebee::duration_t txn_timeout{bumblebee::DEFAULT_TXN_TIMEOUT};
  // How many result rows the interactive shell prints before truncating; 0 shows all. Keeps a giant
  // SELECT from flooding the terminal.
  bumblebee::idx_t max_rows{100};
};

/** @brief Parse a flag's non-negative decimal value. */
static auto ToIdx(const char *value) -> bumblebee::idx_t {
  return static_cast<bumblebee::idx_t>(std::strtoull(value, nullptr, 10));
}

/**
 * @brief Read `argv` into a ShellOptions.
 *
 * An unrecognized argument is ignored, as is a flag whose value is missing — the shell has never
 * rejected its command line, and the e2e harness relies on passing flags this build may not know.
 */
static auto ParseArgs(int argc, char **argv) -> ShellOptions {
  ShellOptions opts;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--memory") == 0 || std::strcmp(argv[i], "-m") == 0) {
      opts.in_memory = true;
    } else if (std::strcmp(argv[i], "--test-protocol") == 0) {
      opts.test_protocol = true;
    } else if (std::strcmp(argv[i], "--no-seed") == 0) {
      opts.no_seed = true;
    } else if (std::strcmp(argv[i], "--prefer-external") == 0) {
      opts.prefer_external = true;
    } else if (i + 1 >= argc) {
      continue;  // every remaining flag takes a value, and there is none left
    } else if (std::strcmp(argv[i], "--db") == 0) {
      opts.db_path = argv[++i];
    } else if (std::strcmp(argv[i], "--max-memory") == 0) {
      opts.max_memory = ToIdx(argv[++i]);
    } else if (std::strcmp(argv[i], "--threads") == 0) {
      opts.max_threads = ToIdx(argv[++i]);
    } else if (std::strcmp(argv[i], "--morsel-pages") == 0) {
      opts.morsel_pages = ToIdx(argv[++i]);
    } else if (std::strcmp(argv[i], "--morsel-size") == 0) {
      opts.morsel_size = ToIdx(argv[++i]);
    } else if (std::strcmp(argv[i], "--frames") == 0) {
      opts.num_frames = static_cast<size_t>(ToIdx(argv[++i]));
    } else if (std::strcmp(argv[i], "--max-rows") == 0) {
      opts.max_rows = ToIdx(argv[++i]);
    } else if (std::strcmp(argv[i], "--txn-timeout") == 0) {
      opts.txn_timeout = std::chrono::milliseconds(std::strtoull(argv[++i], nullptr, 10));
    }
  }
  return opts;
}

/**
 * @brief The SQL of a `-c "<sql>"` one-shot run, or nullptr for an interactive session.
 *
 * Deliberately a separate scan from ParseArgs rather than a case inside it: `-c` must not consume
 * its argument during flag parsing, or a statement like `-c "--memory"` would be read as a flag.
 */
static auto FindOneShotSql(int argc, char **argv) -> const char * {
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

/** @brief Open the database `opts` describes and apply the session settings to it. */
static auto MakeInstance(const ShellOptions &opts) -> std::unique_ptr<bumblebee::BumbleBeeInstance> {
  bumblebee::DatabaseConfig config;
  config.transaction_timeout_ = opts.txn_timeout;
  config.frames_ = opts.num_frames;
  config.prefer_external_ = opts.prefer_external;
  config.max_memory_ = opts.max_memory;
  config.worker_threads_ = opts.max_threads;
  config.morsel_pages_ = opts.morsel_pages;
  config.morsel_size_ = opts.morsel_size;
  auto instance = opts.in_memory ? std::make_unique<bumblebee::BumbleBeeInstance>(config)
                                 : std::make_unique<bumblebee::BumbleBeeInstance>(opts.db_path, config);
  // Seed the demo tables only when the catalog is empty and seeding is not suppressed — always for an
  // in-memory instance, and for a brand-new file so reopening an existing database does not fail on a
  // duplicate CREATE. The e2e harness passes `--no-seed` so each `.slt` file starts from a clean catalog.
  if (!opts.no_seed && instance->GetCatalog().GetTableNames().empty()) {
    instance->GenerateMockTable();
  }
  return instance;
}

/**
 * @brief Load the persistent line-edit history, returning the file it should be saved back to.
 *
 * Best-effort: a read-only or absent HOME just means the session gets no saved history.
 */
static auto LoadHistory() -> std::string {
  std::string history_path;
  if (const char *home = std::getenv("HOME"); home != nullptr) {
    history_path = std::string(home) + "/.bumblebee_history";
    linenoiseHistoryLoad(history_path.c_str());
  }
  linenoiseHistorySetMaxLen(1000);
  return history_path;
}

/** @brief The line loop itself, shared by the interactive and `--test-protocol` shells. */
static void RunReplLoop(bumblebee::BumbleBeeInstance &instance, const ShellOptions &opts,
                        const std::string &history_path) {
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
      RunStatement(instance, line, opts.test_protocol, opts.max_rows);
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

    RunStatement(instance, trimmed, opts.test_protocol, opts.max_rows);
    query.clear();
  }
}

/** @brief The interactive shell: banner and saved history, then the line loop. */
static void RunRepl(bumblebee::BumbleBeeInstance &instance, const ShellOptions &opts) {
  // The banner and persistent history are shell niceties; the e2e harness wants a clean,
  // side-effect-free stdout, so both are skipped in `--test-protocol` mode.
  if (opts.test_protocol) {
    RunReplLoop(instance, opts, "");
    return;
  }
  std::cout << "BumbleBeeDB (" << (opts.in_memory ? "in-memory" : opts.db_path.string())
            << ") — type \\help for help, Ctrl-D to exit." << std::endl;
  RunReplLoop(instance, opts, LoadHistory());
}

auto main(int argc, char **argv) -> int {
  // Exception's constructor traces to stderr in debug builds, which is useful when a
  // test blows up but is just noise here: the shell reports the error itself.
  bumblebee::global_disable_exception_print.store(true);

  const ShellOptions opts = ParseArgs(argc, argv);
  auto instance = MakeInstance(opts);

  // `-c "<sql>"` runs one statement and exits, which makes the shell scriptable.
  if (const char *sql = FindOneShotSql(argc, argv); sql != nullptr) {
    return RunStatement(*instance, sql, opts.test_protocol, opts.max_rows) ? 0 : 1;
  }

  RunRepl(*instance, opts);
  return 0;
}
