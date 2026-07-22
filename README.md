# BumbleBeeDB

BumbleBeeDB is a relational database written from scratch in C++20. It has a
vectorized execution model (DuckDB-style `DataChunk` / `Vector` types), a SQL
front end (parser, binder, planner, optimizer) built on DuckDB's fork of
`libpg_query`, MVCC-based concurrency control, and a persistent on-disk storage
engine.

## Requirements

- A C++20 compiler (Clang or GCC)
- CMake **3.31** or newer
- On Linux: `libuuid` development headers (`uuid-dev`). Not needed on macOS.

Third-party dependencies (`fmt`, GoogleTest) are fetched automatically at
configure time via CMake `FetchContent`; `libpg_query` is vendored in-tree under
`third_party/`.

## Building

Configure and build a Release binary:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces two executables in `build/`:

- `BumbleBee` — the interactive SQL shell
- `unit_tests` — the test suite

### Debug builds and sanitizers

Debug builds are compiled with a sanitizer enabled. Pick one with `-DSANITIZER`
(they are mutually exclusive):

```bash
# AddressSanitizer (default in Debug) — memory bugs
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=address
cmake --build build-debug -j

# ThreadSanitizer — data races / deadlocks (concurrency work)
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=thread
cmake --build build-tsan -j
```

Release builds have no sanitizer and disable asserts.

## Running

Start the interactive shell:

```bash
./build/BumbleBee
```

By default the shell is durable and backed by `bb.db` in the current directory;
the catalog and data survive across runs. A statement may span multiple lines
and is executed when a line ends in `;`. Type `\help` for meta-commands and
press `Ctrl-D` to exit.

Command-line options:

| Option | Description |
|---|---|
| `--db <path>` | Use `<path>` as the database file instead of `bb.db`. |
| `--memory`, `-m` | Run a purely in-memory instance that persists nothing. |
| `-c "<sql>"` | Run a single statement, print the result, and exit (scriptable). |
| `--txn-timeout <ms>` | How long a transaction may stay open before `\gc` aborts it (default 2 hours). Runtime-configurable so tests can shrink it to milliseconds. |

Useful meta-commands (`\help` lists them all):

- `\session <name>` — switch to a named session (created on first use). Each
  session can hold its own open transaction, so concurrent transactions can be
  exercised from one shell: `BEGIN` in `s1`, switch to `s2`, and observe MVCC
  snapshot isolation and write-write conflicts between them.
- `\gc` — run transaction garbage collection: reclaims old row versions and
  aborts any transaction open longer than the timeout. GC is the *only* timeout
  driver today (nothing runs it in the background), which keeps timeout
  behavior deterministic and testable.

Examples:

```bash
# One-shot query against an in-memory instance
./build/BumbleBee -m -c "SELECT 1 + 1;"

# Open a specific database file
./build/BumbleBee --db /tmp/my.db
```

## Testing

Tests use GoogleTest and are registered with CTest. After building, run the
whole suite with CTest:

```bash
cd build && ctest --output-on-failure
```

Or run the test binary directly (supports GoogleTest filters):

```bash
./build/unit_tests
./build/unit_tests --gtest_filter='Catalog*'
```

Concurrency tests are most meaningful under ThreadSanitizer — run them from a
TSan build:

```bash
./build-tsan/unit_tests --gtest_filter='*Concurrent*:*Mvcc*'
```

### End-to-end SQL tests (`.slt`)

`test/e2e/` holds a pytest harness that runs sqllogictest-style `.slt` files (SQL
scripts with expected output) against the `BumbleBee` shell — one pytest test per
file, grouped by feature (`aggregates/`, `joins/`, `dml/`, …). Each file runs
against a fresh in-memory instance, so tests start from an empty catalog.

```bash
# Build the shell, then run the whole corpus (defaults to build/BumbleBee).
cmake --build build --target BumbleBee
python -m pytest test/e2e -v

# One category or one file.
python -m pytest test/e2e -v -k joins
python -m pytest test/e2e/slt/joins/inner_join.slt -v
```

To exercise the vectorized engine's multi-chunk paths without thousands of rows,
build a **small-vector** variant (a lowered `STANDARD_VECTOR_SIZE`, which is a
compile-time constant) and point the harness at it:

```bash
cmake -S . -B build-smallvec -DCMAKE_BUILD_TYPE=Release -DBBDB_VECTOR_SIZE=4
cmake --build build-smallvec --target BumbleBee
BBDB_SLT_BIN=build-smallvec/BumbleBee BBDB_SLT_SMALL_VECTOR=1 python -m pytest test/e2e -v
```

Per-file `# config:` directives lower runtime knobs (morsel size, memory budget,
threads) so a handful of rows can cover the same code paths. See
[`test/e2e/README.md`](test/e2e/README.md) for the `.slt` format and directives.