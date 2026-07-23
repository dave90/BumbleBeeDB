# BumbleBeeDB

BumbleBeeDB is a relational database written from scratch in C++20. It has a
vectorized execution model (DuckDB-style `DataChunk` / `Vector` types), a SQL
front end (parser, binder, planner, optimizer) built on DuckDB's fork of
`libpg_query`, MVCC-based concurrency control, and a persistent on-disk storage
engine. Besides its native row-format tables it supports **external parquet
tables** (Databricks-style): a folder of parquet files queried and written
through plain SQL.

## Requirements

- A C++20 compiler (Clang or GCC)
- CMake **3.31** or newer
- On Linux: `libuuid` development headers (`uuid-dev`). Not needed on macOS.

Third-party dependencies (`fmt`, GoogleTest) are fetched automatically at
configure time via CMake `FetchContent`; `libpg_query` and the parquet stack
(`thrift`, `snappy`, `zstd`, `miniz`, `utf8proc`) are vendored in-tree under
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
- `\vacuum <table>` — sweep an external parquet table's folder: removes part
  files no manifest references (leftovers of a crashed rewrite) and superseded
  manifest versions.

Examples:

```bash
# One-shot query against an in-memory instance
./build/BumbleBee -m -c "SELECT 1 + 1;"

# Open a specific database file
./build/BumbleBee --db /tmp/my.db
```

## External parquet tables

A table can live outside the database file, as parquet files in a folder you
point it at:

```sql
-- Declare the schema yourself (must match any files already in the folder) ...
CREATE TABLE events (id BIGINT, payload VARCHAR)
WITH (format = 'parquet', location = '/data/events');

-- ... or let an empty column list infer it from the files found there.
CREATE TABLE events () WITH (format = 'parquet', location = '/data/events');
```

External tables are queried like any other table — joins with heap tables,
aggregates, filters (with whole parquet row groups skipped via their min/max
statistics when a predicate provably excludes them, and unread columns never
decoded at all). They are writable too, with **non-transactional, copy-on-write**
semantics:

- `INSERT` appends a new part file; `UPDATE` / `DELETE` rewrite only the part
  files that contain matched rows. Deleting every row leaves a valid empty
  table.
- The commit point is an atomic swap of a manifest file (`_bbdb_manifest.N`)
  listing the live part files; readers always see either the old or the new
  state, and a crashed half-finished rewrite is invisible.
- One writer at a time per table: a second concurrent writer fails immediately
  (`concurrent modification of external table ...`) instead of waiting.
- Writes are refused inside `BEGIN ... COMMIT` — a `ROLLBACK` could never undo a
  file rewrite. Reads inside transactions are fine (statement-level snapshots).
- `DROP TABLE` removes only the catalog entry; the data files are yours and are
  never deleted.

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

By default the corpus runs against in-memory instances. Run the same files
**file-backed** — exercising real disk IO and catalog persistence, plus the
`persistence/` tests that restart the shell mid-file to assert data survives a
reopen — with:

```bash
BBDB_SLT_DURABLE=1 python -m pytest test/e2e -v
```

Per-file `# config:` directives lower runtime knobs (morsel size, memory budget,
threads) so a handful of rows can cover the same code paths. See
[`test/e2e/README.md`](test/e2e/README.md) for the `.slt` format and directives.