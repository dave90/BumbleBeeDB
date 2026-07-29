# BumbleBeeDB benchmark suite

Head-to-head performance benchmarks for BumbleBeeDB against established engines:

* **OLAP query performance vs DuckDB** — TPC-H + ClickBench, over parquet.
* **DML performance vs SQLite** — bulk load, predicate update/delete, point ops,
  transaction overhead.

The harness is config-driven and ported from the sibling BumbleBee (datalog)
benchmark runner. Each engine is one JSON config: an untimed `setup` phase, then a
list of single-statement `tests`, each run a few times and timed by wall-clock.
Results are written to a timestamped CSV under `results/` with the delta vs the
comparison engine and vs this config's own previous run.

## Layout

```
benchmark_runner.py     the harness (config in, results CSV out)
prepare_data.py         builds fixtures + generated setup/DML scripts under data/
configs/                one JSON per engine (checked in)
  duckdb_olap.json          OLAP reference
  bumblebeedb_olap.json     OLAP under test  (comparison_config -> duckdb_olap)
  sqlite_dml.json           DML reference
  bumblebeedb_dml.json      DML under test   (comparison_config -> sqlite_dml)
queries/olap/           engine-neutral query bodies (checked in)
  clickbench/ q*.sql
  tpch/       q*.sql
data/                   generated, gitignored: parquet symlinks, setup + DML SQL, db files
results/                generated, gitignored: timestamped CSVs + saved reference outputs
```

Query bodies are **engine-neutral**: they reference logical table names (`hits`,
`lineitem`, …). The per-engine setup binds those names to the *same* parquet bytes —
an external table for BumbleBeeDB (`CREATE TABLE t () WITH (format='parquet',
location=…)`, which adopts a foreign parquet folder and infers the schema), a view
for DuckDB. So both engines read the identical files; the comparison is honest.

## Prerequisites

* **BumbleBeeDB, built Release.** Debug builds carry a sanitizer and are ~100× slower
  — useless for timing. Build it and point the harness at it:
  ```bash
  cmake -S .. -B ../build -DCMAKE_BUILD_TYPE=Release && cmake --build ../build -j
  export BBDB_BIN=../build/BumbleBee     # the default the configs assume
  ```
* **DuckDB CLI** and **SQLite CLI**. Overridable via `DUCKDB_BIN` / `SQLITE_BIN`
  (defaults: `$HOME/.duckdb/cli/latest/duckdb`, `sqlite3`).
* **Parquet fixtures.** By default taken from the sibling datalog repo's downloads
  (`~/git/BumbleBee/benchmarks/downloads/{hits.parquet,tpch/*.parquet}`). Override the
  root with `BBDB_BENCH_FIXTURES=/path/to/parquet_root`.

## Running

Run everything from this `benchmarks/` directory.

```bash
python3 prepare_data.py                                   # once: fixtures + generated scripts

# OLAP: reference first (it saves the expected outputs), then the engine under test.
python3 benchmark_runner.py configs/duckdb_olap.json
python3 benchmark_runner.py configs/bumblebeedb_olap.json

# DML: same order.
python3 benchmark_runner.py configs/sqlite_dml.json
python3 benchmark_runner.py configs/bumblebeedb_dml.json
```

Useful flags: `--only q00,tpch_q03` runs a subset; `--skip-setup` reuses the db from a
previous run. `BBDB_BENCH_DML_ROWS=200000 python3 prepare_data.py` scales the DML load.

## Reading the results

The console prints avg/min per test plus deltas; the CSV has the full detail. The
`match` column:

| value       | meaning                                                                   |
|-------------|---------------------------------------------------------------------------|
| `ref`       | the reference engine's run (it defines the expected output)               |
| `match`     | output equals the reference engine's (order-independent, whitespace-norm) |
| `mismatch`  | ran, but output differs — often just string quoting/number formatting     |
| `ERROR`     | failed unexpectedly — a real gap worth filing                             |
| `xfail`     | failed as expected (feature not implemented yet — see below)              |
| `xfail-PASS`| an xfail test **succeeded** — the feature landed; drop the `xfail` marker  |
| `-`         | correctness not compared (all DML tests: no comparable rows)              |

## Ported-but-unsupported queries (xfail)

The full TPC-H + ClickBench corpus is ported, **including queries that use SQL
BumbleBeeDB does not implement yet**, so the suite already exercises them the day a
feature lands. These carry an `"xfail": "<feature>"` marker in the config: the harness
still runs and times them, but an error is expected, not a regression. When the marker
turns into `xfail-PASS`, the feature works — remove the marker.

Current xfail features, ordered by how many queries they block (from the 2026-07-23
Release run):

* **`ORDER BY <alias>`** — the biggest gap: ordering by an aliased/computed output
  column (`ORDER BY count DESC`, `ORDER BY revenue DESC`) fails with "column … not
  found"; ordering by a base column in the SELECT list works. Blocks clickbench
  q07/q12/q14/q30/q32/q33/q36 and tpch q03/q05/q10.
* `AVG` — clickbench q02/q03/q30/q32, tpch q01/q17
* `LIKE` — clickbench q20/q21, tpch q02
* scalar / correlated subqueries (`T_SubLink`) — tpch q02/q04/q15/q17
* `printf` — clickbench q03
* `IN` — DML `point_in` (batched point update)

## Baseline results (2026-07-23, Release, cold per-query)

Against `hits.parquet` (≈100M rows) and TPC-H SF1, vs the DuckDB reference:

* **6 queries produce output verified equal to DuckDB**: clickbench q00, q01, q06,
  q19, q25, q26. BumbleBeeDB is *faster* than DuckDB's cold run on q00 (0.74s vs
  3.28s) and q06, slower on the point-lookup q19 (2.0s vs 0.16s — it full-scans the
  external parquet, no index) and the ORDER BY … LIMIT scans q25/q26.
* **18 queries are `xfail`** on the features above — they parse-fail in ~50ms and
  their timings are not meaningful.
* **1 genuine `ERROR`**: `tpch_mq07` (a 6-way join) does not finish — capped at a
  60s/1-try timeout here. DuckDB runs it in 0.2s, so this is a real join
  execution/scaling bug, not a missing feature. Left un-`xfail`ed and visible.

Re-run after implementing a feature: an `xfail` flipping to `xfail-PASS` means it
landed (drop the marker); a `mismatch` appearing means the output is wrong.

## Notes & caveats

* **Cold cache, per query.** Each test is a fresh process (BumbleBeeDB `-c`, DuckDB
  `-c`, SQLite stdin), so every query pays startup + a cold buffer pool / cold parquet
  read. This is deliberately symmetric — DuckDB gets a fresh process too — but it means
  these numbers measure cold single-query latency, not warm/repeated execution.
* **DML mutates in place.** DML tests run as a sequence against one seeded table;
  mutating ops use `num_tries: 1` so repeated runs don't compound. `txn_autocommit` vs
  `txn_single` isolates per-commit overhead (N autocommits vs the same N in one txn).
* **BumbleBeeDB's DML config uses the `stdin` driver**, exactly like the SQLite reference.
  The `run` driver would spell the statement as `-c "$(cat $FILE)"`, and for a large DML file
  the shell's command substitution becomes the measurement: a 475 KB `load.sql` costs ~0.30s
  of `zsh` before the engine is even exec'd (`time /usr/bin/true "$(cat data/dml/load.sql)"`
  reproduces it with no database involved). SQLite, fed `< $FILE`, pays none of that — so the
  `run` driver was timing our shell against their engine. The OLAP configs keep `-c` because
  *both* engines use it there and the query files are ~1 KB.

### DML sourced from a join (`*_join`)

`load`/`update_pred`/`delete_pred` and friends write literals, so they only ever measure the
write path. The `*_join` tests drive the same three operations from a two-table join
(`fact` 20k rows ⋈ `dim` 1k rows, both seeded untimed in setup), so one statement pays for a
scan + hash join **and** the write — the shape a real ETL statement has:

| test | statement |
|------|-----------|
| `dml_insert_join` | `INSERT INTO bench SELECT … FROM fact, dim WHERE fact.bucket = dim.b AND dim.w < 500` |
| `dml_update_join` | `UPDATE bench SET v = v + 1 WHERE v > (SELECT avg(dim.w) FROM fact, dim WHERE …)` |
| `dml_delete_join` | `DELETE FROM bench WHERE v > (SELECT max(dim.w) FROM fact, dim WHERE …)` |
| `dml_update_join_in` | `UPDATE bench SET v = v + 1 WHERE k IN (SELECT fact.fk FROM fact, dim WHERE …)` |
| `dml_delete_join_in` | `DELETE FROM bench WHERE k IN (SELECT fact.fk FROM fact, dim WHERE …)` |

The last two are the row-matching shape: the join names the individual rows to write, rather
than collapsing to one aggregate the predicate compares against. They were xfail when the tests
were added — BumbleBeeDB only accepted an IN/EXISTS subquery in a *SELECT's* WHERE — and were
what prompted the fix that flattens such a conjunct into a SEMI/ANTI join for UPDATE/DELETE too.
Note that a semi join is load-bearing here, not an inner one: `fact` has several rows per bucket,
and a target row matched N times must still be written once.

Since the `match` column is `-` for every DML test (no comparable rows), the join tests were
checked by hand instead: running the whole sequence on both engines leaves byte-identical
state (`count(*), sum(k), sum(v)` = `9083, 83242524, 2488827` on each).
* **Only parquet.** BumbleBeeDB has no CSV reader, so only the parquet OLAP variants
  are included (the datalog suite's CSV variants are dropped).
