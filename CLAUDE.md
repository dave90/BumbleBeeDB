# CLAUDE.md

BumbleBeeDB is a relational database written from scratch in C++20: a SQL front end
(parser/binder/planner/optimizer on a vendored `libpg_query`), a vectorized push-based execution
engine (DuckDB-style `DataChunk`/`Vector`), MVCC concurrency, and a persistent row-format storage
engine with a buffer pool. Single process, embedded + interactive shell.

## Build

```bash
# Release (no asserts, no sanitizer)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# Debug builds always carry a sanitizer: address (default) or thread
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=address
cmake -S . -B build-tsan  -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=thread

# Small-vector build: STANDARD_VECTOR_SIZE=4 instead of 1024 (compile-time constant),
# used to exercise multi-chunk code paths with a handful of rows in e2e tests
cmake -S . -B build-smallvec -DCMAKE_BUILD_TYPE=Release -DBBDB_VECTOR_SIZE=4
```

Targets: `bumblebee_lib` (everything), `BumbleBee` (interactive shell), `unit_tests` (GoogleTest).

**Gotcha:** sources are collected with CMake `GLOB_RECURSE`. After **adding or removing a file**,
re-run `cmake .` in the build dir (or `cmake -S . -B <dir>`) or the new file silently isn't built
(symptom: "undefined symbol" at link, or 0 tests matching a new test file).

## Test

```bash
# Unit tests (GoogleTest). Single test: --gtest_filter
./build/unit_tests
./build/unit_tests --gtest_filter='TopNHeapTest*'

# Concurrency/MVCC tests are only meaningful under TSan
./build-tsan/unit_tests --gtest_filter='*Concurrent*:*Mvcc*'

# End-to-end SQL tests: pytest harness running .slt files against the shell
cmake --build build --target BumbleBee
python3 -m pytest test/e2e -q                       # whole corpus
python3 -m pytest test/e2e -k joins -v              # one category
python3 -m pytest test/e2e/slt/order_limit/topn.slt # one file

# Same corpus against the small-vector build (multi-chunk paths; also unlocks
# the `# require: small_vector` files)
cmake --build build-smallvec --target BumbleBee
BBDB_SLT_BIN=build-smallvec/BumbleBee BBDB_SLT_SMALL_VECTOR=1 python3 -m pytest test/e2e -q
```

Run the **full matrix** (unit + e2e on both builds) before declaring a change done. Do **not** run
`unit_tests` from the smallvec build — unit tests assume `STANDARD_VECTOR_SIZE == 1024`.

`.slt` format and the `# config:` directives (`max_memory`, `threads`, `prefer_external`,
`morsel_pages`) are documented in `test/e2e/README.md`. Each `.slt` file gets a fresh in-memory
instance (`--memory --no-seed`). Use `query rowsort` whenever the operator doesn't guarantee row
order (hash join / hash aggregate / ties under parallelism).

## Repository map

```
src/include/            all public headers (mirrors src/), path-relative includes
src/binder/             SQL AST -> bound statements (libpg_query parse tree in)
src/planner/            bound statements -> AbstractPlanNode tree
src/optimizer/          plan rewrites: filter pushdown, NLJ->HashJoin, SortLimitAsTopN, ...
src/execution/          physical operators, expression executor, physical plan generator
  operator/{scan,filter*,projection*,join,aggregate,order,persistent,helper*}/
  sort/                 shared sort machinery: TopNHeap, SortEntry + GatherSorted
  spill/*               SpillCollection (buffer-pool-backed scratch for out-of-core ops)
src/parallel/           Executor, Pipeline, PipelineExecutor, task scheduler (morsel-driven)
src/type/               LogicalType, Value, BumbleString, Vector/DataChunk + kernels
src/storage/            row-format TableHeap, pages, buffer pool (ARC), disk, B+tree, MVCC
src/concurrency/        Transaction, TransactionManager, watermark
src/catalog/            catalog (persisted through the Database owner)
src/main/main.cpp       the shell
test/unit/              GoogleTest, mirrors src/ layout; shared helpers in test/unit/include/
test/e2e/slt/           sqllogictest files by category      (* = header-only operator)
```

Reference docs: `DEVELOP.MD` (coding style, enforced), `test/e2e/README.md` (`.slt` format and
config directives), `TODO.md` (open features).

## Architecture in one pass

`BumbleBeeInstance::ExecuteSql` (src/bumblebee_instance.cpp) drives: parse (libpg_query) → bind →
plan → optimize → `PhysicalPlanGenerator` lowers `AbstractPlanNode`s to `PhysicalOperator`s →
`PipelineBuilder` cuts the operator tree into pipelines at **pipeline breakers** → the `Executor`
runs pipelines with morsel-parallel tasks → a `PhysicalResultCollector` sinks the result.

- An operator is a **source** (`GetData`), a **sink** (`Sink`/`Combine`/`Finalize`), and/or a
  streaming operator (`Execute`). `IsSink() == true` ⟺ pipeline breaker.
- Sink pattern: per-task lock-free `LocalSinkState`, merged into `GlobalSinkState` under a mutex in
  `Combine`, post-processed once in `Finalize`. Source state pattern mirrors it. These state
  structs are file-local in each operator's `.cpp`.
- State lifetime: `Executor` owns all global sink states for the whole query, so a source may
  safely emit chunks that *reference* (dictionary/slice) data owned by its sink state — the
  result collector clones + normalifies every batch it stores.
- Write sinks (Insert/Update/Delete) apply chunks via MVCC (`storage/mvcc/mvcc.h`) directly in
  `Sink`; first-committer-wins on write-write conflicts (throws `ExecutionException`).

### The vector layer (`src/type/vector/`)

The execution currency is `DataChunk` = N equal-length `Vector`s + cardinality (≤
`STANDARD_VECTOR_SIZE` rows). A `Vector` has an encoding (`FLAT`, `CONSTANT`, `DICTIONARY`,
`SEQUENCE`) and never owns memory (a `VectorDataMngr` does), so referencing/slicing is free.
Key idioms — prefer these over anything per-row:

- `Orrify()` → uniform `(data, sel, validity)` triple to write one kernel loop for any encoding.
- `Normalify()` → materialize to flat. `Reference`/`Slice` → zero-copy subset/projection.
- `VectorOperations::Copy(src, dst, sel, target_sel, ...)` → batched selection copy; it re-homes
  string payloads into the target's heap (LIST/ARRAY too), so target never dangles.
- Strings are `string_t` (`BumbleString`): 24-byte handle, ≤11 bytes inlined, payload bytes live
  in a `StringHeap`/vector aux heap — a `string_t` is a *view*; keep its owner alive.
- `CreateSortKey` encodes ORDER BY columns into memcmp-comparable byte strings (NULLs: last in
  ASC, first in DESC). All ordering comparisons in the engine go through these keys.
- `DataChunk::EstimatedBytes()` is the memory-accounting helper (counts string/list payloads);
  operators reserve against `ClientContext::mem_` (`QueryMemoryManager`) with it.

**Storage is row-format, processing is vectorized.** `TableStorage`/`TableHeap` scan/insert/
update/delete in `DataChunk`s; the internal `Tuple`/`RowLayout` representation never leaks into
the execution layer. Out-of-core operators (`PhysicalExternalMergeSort`, `PhysicalGraceHashJoin`)
are **separate operators**, not spill paths inside the in-memory ones: the in-memory variant
throws `MemoryLimitException` when its budget reservation fails and the driver re-lowers that one
node and retries (`prefer_external` forces the out-of-core variants up front).

### Reusable execution building blocks

- `execution/sort/top_n_heap.h` — bounded top-n heap (columnar payload, StringHeap-owned keys,
  integer first-key prefilter). `execution/sort/sorted_gather.h` — `SortEntry` + `GatherSorted`
  (vectorized "materialize rows in sorted order", used by Sort and the external sort's runs).
- `execution/prl_hash_table.h` — the join/aggregate hash table.
- `execution/spill/spill_collection.h` — buffer-pool-backed scratch rows; `Free()` eagerly.

## Coding conventions (enforced; see DEVELOP.MD)

- Names: `CamelCase` classes/methods, `lower_case` variables, members with **trailing underscore**
  (`data_`) — including members of file-local operator state structs; `UPPER_CASE` constants.
- **Trailing return types** everywhere: `auto Foo() const -> int`. Modern C++ (`auto`, range-for).
- Every file starts with the BumbleBee banner comment block; headers use `#pragma once`;
  project includes are path-relative from `src/include` (`#include "type/value.h"`).
- Doxygen `/** @brief ... @param ... @return */` on public APIs. Comments state constraints and
  *why*, not what-the-next-line-does.
- **Vectorize by default.** In operators, per-row `Value`/`GetValue`/`SetValue` loops are a code
  smell — use selection vectors, batched `Copy`, `Reference`/`Slice`, encoded sort keys. `Value`
  is fine at the boundaries (tests, result printing, constants).
- Exceptions from `common/exception.h`; `BUMBLEBEE_ASSERT` for invariants (compiled out in
  Release).

## SQL surface: supported & known limitations

Supported: CREATE/DROP TABLE, INSERT/UPDATE/DELETE, SELECT with joins (inner/left), GROUP BY +
aggregates, DISTINCT, ORDER BY (multi-key ASC/DESC), LIMIT, subqueries, BEGIN/COMMIT/ROLLBACK
(autocommit otherwise), EXPLAIN (incl. physical), VARCHAR/INT/BIGINT/... types, NULLs.

Current limitations to keep in mind (several are pinned by tests in `test/e2e/slt/unsupported/`):

- No `OFFSET` (LimitPlanNode carries only the limit).
- `ORDER BY` can only reference columns in the SELECT list ("column does not exist" otherwise).
- `NULLS FIRST/LAST` is parsed but **ignored**; the fixed convention is NULLs last in ASC, first
  in DESC.
- `ORDER BY x LIMIT n` always collapses to TopN (`OptimizeSortLimitAsTopN`) — bounded memory,
  never spills, no cost-based cutoff for huge n.
- Every table has a primary key: a visible auto `BIGINT _id` (column 0, usable unquoted) is added
  when none is declared; a B+tree index enforces PK uniqueness; index maintenance is INSERT-only.
- Durability is clean-shutdown only (no WAL yet). Persistent instances write `bb.db` (or `--db`).
- Concurrency: MVCC snapshot isolation by default, Serializable via commit-time validation; no
  lock manager. Transaction timeout is GC-driven only (`\gc` / `--txn-timeout`); nothing runs GC
  in the background.
- `INSERT INTO t SELECT ... FROM t` (the insert's own target table) uses statement-snapshot
  semantics: the scan sees only rows present when it started, so a self-insert doubles the table
  (no Halloween problem). NULL is an untyped literal that fits any column type; `IS [NOT] NULL`
  is supported; integer literals up to the int64 range bind as BIGINT.

## Shell (useful when debugging by hand)

`./build/BumbleBee` opens durable `bb.db` in cwd; `\help` for meta-commands. Flags: `--memory/-m`,
`--db <path>`, `-c "<sql>"` (one-shot), `--no-seed`, `--max-memory <bytes>`, `--threads <n>`,
`--prefer-external`, `--morsel-pages <n>`, `--frames <n>`, `--txn-timeout <ms>` (transaction
timeout, default 2h — runtime-configurable, no special build), `--test-protocol` (machine-readable,
used by the e2e harness). `EXPLAIN <q>` / `EXPLAIN (physical) <q>` shows plans without running.
`\session <name>` switches between named sessions (each can hold its own open transaction —
how the e2e concurrent-transaction tests interleave deterministically); `\gc` drives
`TransactionManager::GarbageCollection()`, the only transaction-timeout enforcer (nothing runs GC
automatically).

## Workflow expectations for changes

1. Mirror placement: implementation in `src/<area>/`, header in `src/include/<area>/`, unit test
   in `test/unit/<area>/`.
2. New/changed operator behavior needs **both** a unit test and `.slt` coverage; run the e2e
   corpus on the normal **and** small-vector builds (chunk-boundary bugs only show on the latter).
3. Concurrency-touching changes (MVCC, txn manager, parallel sinks) must pass under the TSan
   build.
4. When adding a sink operator, follow the local/combine/finalize pattern of the existing ones
   (per-task lock-free local state, merge under the global mutex in `Combine`); reserve memory
   via `EstimatedBytes()` if the operator materializes unbounded input.
