# End-to-end SQL tests (`.slt`)

A pytest harness that runs sqllogictest-style `.slt` files against the `BumbleBee` shell, in the
spirit of bustub's `test/sql`. Each `.slt` file is one pytest test; files are grouped into
category subfolders under `slt/`.

## Running

```sh
# Build the shell first.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

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

sleep 300               # pause for 300 ms (used by the transaction-timeout tests)
```

- Result columns are space-separated; `NULL` renders literally as `NULL`; strings render quoted
  (`'alice'`).
- `DELETE` / `UPDATE` / `INSERT` used as a `query` compare against the affected-row count.
- Use `rowsort` whenever the operator (hash aggregate / hash join) does not guarantee row order.
- A record's SQL may also be a shell meta-command (e.g. `query` + `\gc`).

### Connection labels (concurrent transactions)

A `statement` / `query` directive may carry a connection label — an extra token naming a shell
session (`default` when omitted):

```
statement ok s1
BEGIN;

statement ok s1
INSERT INTO t VALUES (1);

# s2 must not see s1's uncommitted row
query s2
SELECT COUNT(*) FROM t;
----
0
```

The runner emits `\session <label>` (silent) whenever the label changes, so each session holds its
own open transaction inside the single shell process. Statements still execute one at a time —
interleavings are fully deterministic; the concurrency semantics (snapshot visibility,
first-committer-wins conflicts) come from MVCC. True thread-level races belong in the TSan unit
tests, not here.

## Directives (`#` comments)

```
# config: morsel_pages=4 max_memory=65536 threads=1 prefer_external=true
# seed: mock            # seed the demo tables before running (default: empty catalog)
# require: small_vector # skip unless run against the small-vector build
# require: durable      # skip unless run file-backed (BBDB_SLT_DURABLE=1)
# fixture: t1.parquet   # copy these files from test/e2e/fixtures/ into the file's ${TMPDIR}
# restart               # cleanly shut the shell down and relaunch it on the same database file
```

Every file also gets a fresh scratch folder; the literal `${TMPDIR}` in any statement expands to
its path (removed after the run). External parquet tables use this for their `location`, with
`# fixture:` staging pre-existing data files into it first.

## Storage modes

By default every file runs against an in-memory instance (`--memory --no-seed`). Setting
`BBDB_SLT_DURABLE=1` runs the SAME corpus file-backed instead (`--db ${TMPDIR}/bb.db`), so the
on-disk storage layer — real file IO, page eviction to disk, catalog serialization at shutdown —
is exercised without duplicating any test:

```bash
BBDB_SLT_DURABLE=1 python -m pytest test/e2e -v
```

Reopen semantics (data surviving a process restart) live in `persistence/`: those files use the
`# restart` record, which cleanly shuts the shell down (clean shutdown is what serializes the
catalog — there is no WAL) and relaunches it on the same database file. `# restart` is only
meaningful file-backed, so such files must declare `# require: durable` (the parser enforces it);
they are skipped in memory runs. Note that named sessions and open transactions reset with the
process — the transaction files pin exactly that.

`# config:` values are translated into `BumbleBee` CLI flags when the file's process launches. Only keys
with an observable effect are accepted; an unknown key fails the test loudly (typo protection). Supported:

| key | effect |
|-----|--------|
| `morsel_pages` | heap pages per parallel-scan morsel — lower it to force multi-morsel scans |
| `max_memory` | per-query memory budget (bytes) before an out-of-core operator spills |
| `threads` | worker-thread cap for the query |
| `prefer_external` | `true` forces the out-of-core join/sort variants |
| `txn_timeout` | transaction timeout in **milliseconds**, enforced when a record runs `\gc` — runtime-configurable (unlike the compile-time vector size), so timeout tests need no dedicated build |

Deliberately not exposed yet (they would be silent no-ops): `morsel_size` (no consumer until the columnar
scan lands) and `frames` (the in-memory backend ignores it).

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
