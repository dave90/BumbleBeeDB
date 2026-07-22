# End-to-end SQL tests (`.slt`)

A pytest harness that runs sqllogictest-style `.slt` files against the `BumbleBee` shell, in the
spirit of bustub's `test/sql`. Each `.slt` file is one pytest test; files are grouped into
category subfolders under `slt/`.

## Running

```sh
# Build the shell first.
cmake --build build --target BumbleBee

# Run the whole corpus (defaults to build/BumbleBee).
python -m pytest test/e2e -v

# Pick a specific binary (e.g. the small-vector build, see below).
BBDB_SLT_BIN=build-smallvec/BumbleBee BBDB_SLT_SMALL_VECTOR=2 python -m pytest test/e2e -v

# Run one category or one file.
python -m pytest test/e2e -v -k joins
python -m pytest test/e2e/slt/joins/inner_join.slt -v
```

Each file runs against a **fresh in-memory instance** (`--memory --no-seed`), so every test starts
with an empty catalog — no table cleanup needed. State persists across records *within* a file.

## `.slt` format

Records are separated by blank lines. `#` begins a comment.

```
statement ok           # SQL that must succeed
CREATE TABLE t(a INT);

statement error         # SQL that must raise an error
SELECT missing FROM t;

query                   # compare the result against the block after ----
SELECT a FROM t;
----
1
2

query rowsort           # order-insensitive comparison (sort both sides first)
SELECT a FROM t;
----
2
1
```

- Result columns are space-separated; `NULL` renders literally as `NULL`.
- `DELETE` / `UPDATE` / `INSERT` used as a `query` compare against the affected-row count.
- Use `rowsort` whenever the operator (hash aggregate / hash join) does not guarantee row order.

## Directives (`#` comments)

```
# config: morsel_pages=4 max_memory=65536 threads=1 prefer_external=true
# seed: mock            # seed the demo tables before running (default: empty catalog)
# require: small_vector # skip unless run against the small-vector build
```

`# config:` values are translated into `BumbleBee` CLI flags when the file's process launches. Only keys
with an observable effect are accepted; an unknown key fails the test loudly (typo protection). Supported:

| key | effect |
|-----|--------|
| `morsel_pages` | heap pages per parallel-scan morsel — lower it to force multi-morsel scans |
| `max_memory` | per-query memory budget (bytes) before an out-of-core operator spills |
| `threads` | worker-thread cap for the query |
| `prefer_external` | `true` forces the out-of-core join/sort variants |

Deliberately not exposed yet (they would be silent no-ops): `morsel_size` (no consumer until the columnar
scan lands), `frames` (the in-memory backend ignores it), and `txn_limit` (an active-transaction cap is
unobservable while every statement autocommits — it arrives with explicit `BEGIN`/`COMMIT`).

## Lowering engine sizes for multi-chunk coverage

`STANDARD_VECTOR_SIZE` is a compile-time constant (it sizes fixed arrays), so it cannot be a runtime
flag. To exercise multi-chunk paths with only a handful of rows, build a **small-vector** variant and
point the harness at it:

```sh
cmake -S . -B build-smallvec -DBBDB_VECTOR_SIZE=4
cmake --build build-smallvec --target BumbleBee
BBDB_SLT_BIN=build-smallvec/BumbleBee BBDB_SLT_SMALL_VECTOR=1 python -m pytest test/e2e -v
```

(The `-DBBDB_VECTOR_SIZE` option and `# require: small_vector` gating land in Phase 2.)
