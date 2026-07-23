# Plan: External Parquet Tables

Databricks-style external tables: a table whose data lives as Parquet files in a
user-specified folder instead of the native row-format heap. Queryable through the normal
engine; writable with **non-transactional, copy-on-write** semantics (updates delete and
recreate the data files); concurrent writers to the same table conflict and **one fails
immediately**.

Status: ALL PHASES (0–5) implemented and verified (full matrix: unit, e2e normal + small-vector,
TSan). The parquet module lives in `src/storage/parquet/` (+ `src/include/storage/parquet/`).
Phase 4 shipped row-group skipping (zone predicates vs min/max statistics, which our writer now
emits for numeric columns), `\vacuum <table>`, and projection pushdown: `OptimizeColumnPruning`
(the previously stubbed pass) computes per-scan required-column sets and both scan backends skip
unreferenced columns (heap: no row->vector gather; parquet: pages never decoded). Schemas stay
full-width — pruned slots surface as constant-NULL vectors nothing reads — so no column
renumbering happens anywhere (the trap the old stub warned about). Phase 5 shipped CAST/`::` end to end (strict semantics: failed conversions error),
STRING→DATE/TIMESTAMP parsing, calendar/decimal rendering in Value::ToString, and string-literal
coercion into DATE/TIMESTAMP columns on INSERT.
Implementation notes vs the original design:
- File-granular rewrites (planned as a Phase-4 optimization) shipped in Phase 3: DELETE/UPDATE
  rewrite only the part files containing touched RIDs; untouched files carry forward.
- A pre-existing engine bug surfaced and was fixed on the way: hash joins with key pairs of
  different physical widths (e.g. a parquet INT32 column vs a heap BIGINT) silently dropped
  matches for every probe row after the first; the lowering now coerces both keys to a common
  type (`CoerceJoinKeys` in the physical plan generator).

---

## 1. Semantics (the contract)

### DDL

```sql
-- Declared schema: must match the parquet files if the folder is non-empty
CREATE TABLE events (id BIGINT, payload VARCHAR)
WITH (format = 'parquet', location = '/data/events');

-- Schema inference: allowed only when the folder already contains parquet data
CREATE TABLE events () WITH (format = 'parquet', location = '/data/events');
```

- `WITH (format, location)` rides on Postgres storage parameters (`pg_stmt->options`,
  a `PGList` of `PGDefElem`) — **no grammar change needed**. `format` values other than
  `'parquet'` are an error; `location` is required when `format` is given.
- **Adoption:** CREATE on a non-empty folder adopts the existing `*.parquet` files.
  - Columns omitted (`()`): schema is **inferred** from the parquet footer(s) and stored
    in the catalog. All files must agree on the schema; mismatch between files is an error.
    Empty folder + no columns = error ("cannot infer schema from empty location").
  - Columns declared: must **match** the parquet schema — same column count, names
    (case-insensitive), and compatible types — otherwise error at CREATE time.
- External tables get **no** auto `_id` column, no primary key, and no B+tree index.
- `DROP TABLE` removes the catalog entry only; **data files are never deleted** (external
  semantics). Manifest/lock files we created are left behind too (documented).

### Reads

- Statement-level snapshot: a scan reads the newest manifest once at scan start and works
  from that file list for the whole statement. Readers never block and are never blocked.
- No MVCC visibility checks; the manifest snapshot *is* the visibility mechanism.
  Inside an explicit transaction, consistency is per-statement, not per-transaction
  (documented limitation).

### Writes

- `INSERT` = append: write one new part file, commit manifest `N+1` = old list + new file.
- `DELETE` / `UPDATE` = copy-on-write rewrite: read snapshot, drop/patch matched rows,
  write fresh part files, commit manifest `N+1`. **Deleting all rows is legal** and yields
  a manifest with an empty file list — the table exists, has its schema, and scans return
  zero rows. Subsequent INSERTs repopulate it.
- **Non-transactional.** Writes to an external table are **refused inside an explicit
  transaction** (`ExecutionException`, checked where autocommit is decided,
  `src/bumblebee_instance.cpp:444`). In autocommit mode the manifest swap in `Finalize` is
  the commit point; the surrounding bookkeeping txn's commit/abort has no effect on files.
  This mirrors how DDL is already handled outside transactions.
- **Concurrency = writer try-lock, loser fails fast.** One writer at a time per table;
  a second concurrent writer gets `ExecutionException("concurrent modification of external
  table '<name>'")` immediately — no waiting. Readers unaffected.

---

## 2. What already exists

### In this repo (seams are pre-built; gaps are concrete)

| Exists | Where |
|---|---|
| `StorageFormat{ROW, PARQUET}` threaded through catalog | `src/include/catalog/catalog.h:114` (`CreateTable`), `:174` (`LoadTable`) |
| Abstract `TableStorage` base (`MakeScan/Append/Update/Delete/Fetch`) | `src/include/storage/table/table_storage.h:59` |
| `ParquetTable : TableStorage` stub (holds `path_`, all methods throw) | `src/include/storage/table/parquet_table.h:32` |
| `PhysicalOperatorType::PARQUET_SCAN` enumerant | `src/include/execution/physical_operator.h:43` |
| Format byte already serialized in the catalog record | `src/database.cpp:232` |

Gaps:
- `Binder::BindCreate` ignores `pg_stmt->options` entirely (`src/binder/bind_create.cpp:151`).
- **Pre-existing crash:** `CREATE TABLE t () WITH (...)` currently **segfaults** — the
  binder assumes a non-empty `tableElts`. Must be fixed defensively regardless.
- Every DML operator hard-casts to `TableHeap` via a local `ResolveHeap()`
  (`physical_table_scan.cpp:29`, `physical_insert.cpp:35`, `physical_update.cpp:37`,
  `physical_delete.cpp:33`) and the write path is hard-bound to `MvccInsert/Update/Delete`
  (`storage/mvcc/mvcc.h`).
- `Database::SerializeCatalog` unconditionally does `static_cast<TableHeap*>` and reads
  page ids (`src/database.cpp:229`) — crashes for non-heap storage. Needs a format branch
  (persist `location` instead of page ids) and a `kVersion` bump (`database.h:76`).
- No file-format reader/writer or third-party parquet dependency exists here.

### In the donor repo (`~/git/BumbleBee`, the datalog project)

A complete DuckDB-derived parquet layer operating on that project's DuckDB-style
`DataChunk`/`Vector` — small impedance mismatch with ours:

- Reader: `src/common/parquet/ParquetReader.cpp` + `src/include/bumblebee/common/parquet/`
  (ColumnReader hierarchy, `RleBpDecoder`, `ThriftTools`, footer metadata cache,
  `ParquetStatistics` for row-group min/max).
- Writer: `ParquetWriter` — `flush(ChunkCollection&)` / `finalize()`, per-codec compression.
- Vendored deps to bring over: `thrift`, generated `parquet` (parquet_types), `snappy`,
  `zstd`, `miniz` (from `~/git/BumbleBee/third_party/`).
- Test corpus + harness to port: `test/unit/bumblebee/common/parquet_reader/`
  (`data/*.parquet`: snappy/gzip/zstd/decimal/null/lineitem; `generate_expected.sh`
  produces expected CSVs), plus `test/unit/bumblebee/function/{read,write,write_null}_parquet_test.cpp`.

Porting notes: restyle to DEVELOP.MD (trailing return types, `CamelCase` methods —
the donor uses `flush()`/camelCase members), swap its types for ours
(`LogicalType`, `DataChunk`, `Vector`, `string_t`/`StringHeap`), and route file IO through
whatever this repo uses (donor has its own `FileSystem`/`BufferedFileWriter`).

---

## 3. Directory layout & the manifest protocol

```
/data/events/
  part-000001.parquet
  part-000002.parquet
  _bbdb_manifest.3        # newest wins: ordered list of live part files (+ row counts)
  _bbdb_lock              # writer lock file (crash-tolerant, see below)
```

- **Reader protocol:** find the highest-numbered `_bbdb_manifest.N`, read it once, scan
  exactly those files. Files present in the directory but absent from the manifest are
  invisible — which makes a crashed half-finished rewrite harmless by construction.
- **Writer protocol (lost updates structurally impossible):**
  1. acquire the table write lock (throw immediately if held),
  2. *then* read manifest `N` (never base a rewrite on a manifest read before the lock),
  3. write new part files under fresh unique names,
  4. write `_bbdb_manifest.N+1` to a temp name and atomically `rename()` into place —
     this is the commit point,
  5. unlink part files dropped by the rewrite (POSIX keeps them readable for in-flight
     scans holding open fds), release the lock.
- **Adoption / bootstrap:** a folder with parquet files but no manifest is treated as
  "manifest = all `*.parquet` files, sorted"; the first CREATE (or first write)
  materializes `_bbdb_manifest.0`. CREATE on an empty folder with declared columns
  writes an empty `_bbdb_manifest.0` (this is also the "all rows deleted" state).
- **Locking:** in-process `std::mutex` (`try_lock`) on `ParquetTable` is the primary
  mechanism (single-process engine). Additionally take `_bbdb_lock` with
  `O_CREAT|O_EXCL` for the duration of a write to defend against two shell instances on
  the same folder; treat a lock file older than a threshold as stale (crash leak).
- Orphan part files (from crashes) are swept by a later `\vacuum`-style meta-command
  (out of scope for v1; document).

Manifest file format: plain text, one header line (version, schema hash optional), one
line per part file with row count. Human-inspectable on purpose.

---

## 4. Implementation phases

### Phase 0 — Port the parquet layer (self-contained, no engine changes)

- New area `src/storage/parquet/` + `src/include/storage/parquet/` (mirror placement), vendored deps under
  `third_party/`. Public surface kept minimal:
  - `ParquetFileReader(path)` → schema (`vector<Column>`), row-group count, and a
    row-group cursor producing `DataChunk`s (with optional column projection).
  - `ParquetFileWriter(path, schema, codec)` → `Append(DataChunk&)`, `Finalize()`.
  - Parquet↔`LogicalType` mapping table (one place, both directions; unsupported
    parquet types → clear `NotImplementedException` naming the column).
- Remember the CMake `GLOB_RECURSE` gotcha: re-run cmake after adding files.
- **Tests (unit):** port the donor's `parquet_reader` corpus and tests into
  `test/unit/parquet/`; port `read/write/write_null` round-trip tests against our
  `DataChunk`. Copy `generate_expected.sh` for regenerating expected CSVs.
- Exit criterion: round-trip (write → read) equality for all supported types incl. NULLs
  and all three codecs, and correct reads of the foreign-produced corpus files.

### Phase 1 — DDL, catalog, persistence (read-only tables exist)

- `Binder::BindCreate`: parse `pg_stmt->options` → `format_`/`location_` on
  `CreateStatement`; **fix the empty-column-list segfault** (empty `tableElts` is legal
  iff format is external); skip the `_id` prepend (`bind_create.cpp:220`) for external.
- `HandleCreateStatement` (`bumblebee_instance.cpp:272`): schema inference from the
  location when columns are empty; declared-schema validation against files otherwise;
  pass `StorageFormat::PARQUET`, `auto_id=false`, no `CreateIndexForKey`.
- `Catalog::CreateTable/LoadTable`: construct `ParquetTable(path, schema)` in the PARQUET
  branches (currently `throw`). New `location_` field on `TableInfo`.
- `Database::SerializeCatalog` / recovery loop: branch on format — persist location, not
  page ids; bump `kVersion` (old records still load: version-gated read).
- **Tests:** unit (bind errors: bad format value, missing location, schema mismatch,
  inference on empty folder, `_id` absence); e2e create/describe/drop; persistence test
  (create external table in durable instance, reopen, still scannable).

### Phase 2 — Read path

- `PhysicalParquetScan` (new source operator, uses the existing `PARQUET_SCAN` enum),
  selected in `PhysicalPlanGenerator::CreateSeqScan` (`physical_plan_generator.cpp:134`)
  by dispatching on `storage_->GetFormat()`.
- Morsel = **(file, row group)**: `GlobalSourceState` reads the manifest + footers once,
  builds a flat row-group list, `MaxThreads()` = row-group count, locals claim entries
  with `fetch_add`. `GetData` = drain current row-group cursor → claim next. No
  `ParallelScanState` needed. Scan emits a **synthetic RID** per row when requested:
  `(file_index << 32) | row_index` (needed by Phase 3's DML lowering).
- Memory: reserve the decoded row-group footprint against `QueryMemoryManager` via
  `EstimatedBytes()`-style accounting before decoding.
- Empty manifest → `FINISHED` immediately (zero rows, correct schema).
- **Tests:** unit for the RID encoding and morsel claiming; e2e SELECT/joins/aggregates
  over an adopted folder (fixture-provided parquet files), `query rowsort` everywhere
  (parallel row groups don't guarantee order); small-vector build run is mandatory
  (row-group-boundary chunking bugs will only show there).

### Phase 3 — Writes (insert append, delete/update copy-on-write, conflict rule)

- `PhysicalParquetInsert` / `PhysicalParquetDelete` / `PhysicalParquetUpdate` sinks,
  selected in `CreateInsert`/`LowerDmlChild` (`physical_plan_generator.cpp:163,98`) by
  format dispatch. They bypass `ResolveHeap` and all `Mvcc*` calls.
  - Insert: locals buffer chunks; `Combine` merges; `Finalize` writes one part file +
    manifest swap.
  - Delete: rid-emitting `PhysicalParquetScan` + filter (existing `LowerDmlChild` shape);
    sink collects the doomed RID set; `Finalize` streams the snapshot, skips those rows,
    rewrites, swaps. Deleting everything ⇒ empty manifest (legal, tested).
  - Update: sink collects `rid → new row values`; `Finalize` streams the snapshot,
    substitutes patched rows, rewrites, swaps.
- Writer lock exactly as §3 (in-process try-lock + `_bbdb_lock` file), acquired before
  the manifest read the write is based on; conflict throws `ExecutionException`.
- Refuse external-table writes inside an explicit transaction (check at
  `bumblebee_instance.cpp:444`, where autocommit is decided).
- **Tests:**
  - Unit: manifest protocol (writer steps, adoption, empty manifest, stale lock),
    conflict try-lock, RID→row patching.
  - e2e: insert-then-select round trips; `DELETE` all rows → `SELECT` returns empty →
    re-INSERT works; update rewrites; **concurrent conflict via the `\session`
    interleaving trick** (session A begins a slow write, session B's write fails with the
    conflict error — deterministic with the existing e2e concurrent-transaction pattern);
    write-inside-BEGIN rejected. Full run on normal **and** small-vector builds.
  - TSan: concurrent reader + writer, two concurrent writers (one must fail, no data race).

### Phase 4 — Polish (each item independent, post-MVP)

- Row-group skipping via min/max statistics (donor's `ParquetStatistics` ports over) +
  projection pushdown into the reader.
- File-granular rewrites: only rewrite part files containing touched RIDs; carry
  untouched files forward in the manifest (turns UPDATE/DELETE from O(table) into
  O(touched files) — the manifest design gives this nearly for free).
- `\vacuum <table>` meta-command: delete directory files not referenced by the newest
  manifest.
- `EXPLAIN` niceties: show location, file/row-group counts, skipped row groups.

### Phase 5 — CAST expressions (companion work; independent of the parquet phases)

Makes scanned DATE/TIMESTAMP (and DECIMAL) columns actually usable in predicates and
inserts. Most of the chain already exists — only the front is missing:

| Layer | State | Work |
|---|---|---|
| Parse | done | libpg_query emits `T_PGTypeCast` for both `CAST(x AS T)` and `x::T` |
| Bind | **missing** | `BindExpression` throws at `bind_select.cpp:809`; `ExpressionType::TYPE_CAST` enumerant exists but no expression class |
| Plan | **missing** | no `TYPE_CAST` case in `plan_expression.cpp` |
| Execute | done | `execution/expressions/cast_expression.h` exists (already used by `plan_insert.cpp`); kernels `VectorOperations::Cast`/`TryCast` implemented with DECIMAL coverage + tests |

- **Binder:** new `BoundTypeCast` (`src/include/binder/expressions/bound_type_cast.h`:
  child expression + target `LogicalType`), bound from `T_PGTypeCast` (`arg` +
  `typeName`). Reuse `BindColumnDefinition`'s type-name resolution
  (`LogicalType::FromString` + typmods) so `CAST(x AS DECIMAL(10,2))` and
  `CAST(x AS VARCHAR(n))` work identically to DDL — factor that helper out of
  `bind_create.cpp` rather than duplicating it.
- **Planner:** `TYPE_CAST` case in `plan_expression.cpp` lowering to the existing
  `CastExpression`.
- **Semantics:** SQL `CAST` must **error** on a failed conversion.
  `VectorOperations::Cast` turns failures into NULL, so the executor path for explicit
  casts uses `TryCast` and throws `ExecutionException` with the offending value/type in
  the message. (A NULL-on-failure `TRY_CAST` can be exposed later on the same plumbing.)
- **Kernels:** fill gaps in `vector_cast.cpp` for STRING↔DATE/TIMESTAMP (parse/format);
  these are also what unblocks **INSERT literal coercion** — `plan_insert.cpp` already
  wraps VALUES in `CastExpression`s toward column types, so once STRING→TIMESTAMP exists,
  `INSERT INTO t VALUES ('2024-01-01 10:00:00')` into a TIMESTAMP column starts working
  (today it fails schema validation before ever casting — relax that check to "castable",
  not "equal").
- **Tests:** unit for `BoundTypeCast` binding (incl. typmods) and STRING↔DATE/TIMESTAMP
  kernels (round-trip, bad-format error); e2e `.slt` for `CAST`/`::` in SELECT and WHERE,
  cast-failure errors, timestamp literal INSERT + range predicates
  (`WHERE ts > CAST('2024-01-01' AS TIMESTAMP)`), on both normal and small-vector builds.

Sequencing: no parquet phase depends on this, but landing it before (or alongside)
Phase 2 makes the external-table e2e corpus much richer — timestamp/decimal parquet
columns become filterable, not just selectable.

---

## 5. e2e harness addition

`.slt` files need a per-test scratch folder for locations. Plan: the pytest harness
exports a fresh temp dir per file and substitutes `${TMPDIR}` in statements before
feeding them to the shell; fixtures that need pre-existing parquet data copy corpus
files (from the ported Phase-0 test data) into it first. New category
`test/e2e/slt/external/`. Document the directive in `test/e2e/README.md`.

---

## 6. Risks / open questions

- **Vendored dep size:** thrift + snappy + zstd + miniz is the largest third-party
  addition so far. Mitigation: copy only what the donor's parquet layer actually uses
  (the donor already trimmed these).
- **Type coverage:** DECIMAL, DATE, and TIMESTAMP all exist in the type layer (DECIMAL
  is fully wired: binder `DECIMAL(w,s)`, cast/arith/comparison kernels, tests), so the
  parquet type mapping covers them from day one — including the donor's decimal corpus
  files and its `ParquetTimestamp` handling of parquet's INT96/INT64 encodings. The real
  gap is on the **SQL-surface side**, independent of this feature: timestamp string
  literals don't yet coerce on INSERT and `CAST(...)` is unimplemented, so scanned
  TIMESTAMP columns can be selected but are hard to use in predicates until Phase 5
  (CAST expressions) lands — see that phase; it's independent of the parquet phases and
  can go in any order. Parquet types with no LogicalType counterpart throw a clear
  `NotImplementedException` naming the column.
- **RID width:** `(file << 32 | row)` caps a single part file at 2^32 rows and a table at
  2^31 files — fine; assert at write time.
- **Stale `_bbdb_lock` heuristic** (age threshold) is inherently racy across processes;
  acceptable for an embedded single-process-primary engine, documented.
- **Schema evolution: out of scope (TODO, tracked in TODO.md).** v1 pins the schema at
  CREATE: scans validate each file's footer schema against the catalog schema and fail
  with a clear error on mismatch. Drift can only come from external writers (our own
  writes always emit the catalog schema), and unknown files are invisible until adopted
  thanks to the manifest. When picked up later, the valuable slice is **per-file schema
  resolution** in the scan cursor — name-based column matching, missing column →
  `CONSTANT` NULL vector, extra column → not read, widened type → existing cast kernels —
  plus optionally `ALTER TABLE ADD/DROP COLUMN` for external tables only (catalog-only
  change, no data rewrite). Column RENAME detection stays unsupported (needs column-ID
  metadata foreign parquet files don't have).
