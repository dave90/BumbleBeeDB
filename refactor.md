# BumbleBeeDB — Refactoring Plan

Plan of record for the brief in `plan_refactor.md`. Every number is measured, not estimated.

**Prime directive, from the brief:** behaviour, file/wire formats, CLI, and performance are frozen.
A refactor that cannot be *shown* to preserve them does not ship. Where a change provably conflicts
with that, the brief says not to force it — say so and take the safest alternative.

> **On completeness.** I claimed this plan was complete once and was wrong; a second pass found five
> missing workstreams, and a third found two more. The traceability table in §1 is the mechanism for
> checking — every clause of the brief has a row, and a clause with no workstream is a gap. That is
> a better guarantee than my assertion.

---

## 1. Traceability: every clause of the brief → where it is handled

| Brief clause | Status | Where |
|---|---|---|
| Preserve behaviour / formats / CLI / protocols | **held** | §6 protocol, applied at every step |
| Preserve or improve performance | **held** | codegen gates (byte-identity ×5 files), A/B both orders |
| Remove anonymous namespaces | **done** (98→3) | §2, §5 |
| Apply RAII consistently | **already healthy** | §2 |
| Deterministic ownership; avoid unnecessary shared ownership | **done** | W7 — buffer-pool trio single-owner, guards non-owning; the rest verified shared |
| Prefer stack allocation | **done** | W7 — the pool latch is now a plain member |
| Eliminate leaks / dangling / invalid iterators | **already healthy** | §2 (audited) |
| `const` / `constexpr` correctness | **done** | W5 audits (const_cast ×7 benign; tidy carries the rest) |
| `noexcept` only where genuinely non-throwing | **done** | W5 — all on move ops, verified non-throwing |
| Preserve exception guarantees | **done** | W7 shipped under the full matrix; the grow-path is now exception-safe (W5) |
| Prefer standard-library facilities | **done** | W8 — `make_unique_for_overwrite`, `= delete` |
| Avoid unnecessary macros | **done** | W8 — DISALLOW_* expanded, dead MappingType deleted; keeps documented |
| Replace C-style casts | **already healthy** | 7 benign hits |
| Avoid raw `new`/`delete` | **already healthy**; idiom fix in W5 | 0 owning raw pairs |
| Preserve alignment / packing / aliasing / POD-ness | **done** | no layout change anywhere (padding explicitly rejected, W9) |
| No accidental implicit conversions; `explicit` | **done** | W5 — `google-explicit-constructor` wired into CI tidy (local toolchain cannot run it) |
| `[[nodiscard]]`, `[[maybe_unused]]` | **done** | W5 — all 9 `Try*` APIs; 8 intentional discards made explicit |
| Keep header deps minimal; reduce compile overhead | **closed** | W6 — dropped by its own gate (31 % A/A build noise); candidate recorded |
| Avoid moving impl into headers | **done** | nothing moved into headers anywhere in this refactor |
| SOLID / DRY / KISS / YAGNI | **done** | W3 + W4 + §4 closing dispositions |
| Improve naming and organisation | **done** | W3 (build/probe → left/right and the splits) |
| Reduce coupling / increase cohesion | **done** | W3/W4 extractions; S5 disposition recorded |
| Remove dead code and obsolete comments | **DONE** | W10 — 20 dead items removed; TODOs 17 → 9 |
| Eliminate duplicated logic | **done** | W4 — the convertible sites converted (413 → 302 PhysicalType labels); the rest are enumerations, recorded per family |
| Simplify overly complex functions | **done** | W3 — 8 of 13 split; 4 hot-path kept with measured verdicts; depth 10 → 5 |
| Composition over inheritance | **done** | no new hierarchies introduced; S5 disposition recorded |
| Follow existing style | **DONE** | W1 — `.clang-format` + `.clang-tidy` + CI |
| Concurrency: orders, lock ordering, false sharing, lock-free | **done** | W9 — 29 relaxed uses audited (3 safe categories), lock ordering documented, padding rejected as unmeasurable |
| Preserve thread-local behaviour | **done** | 0 uses — nothing to preserve |
| Preserve supported compilers / OS / architectures | ☐ CI | W11 — the one item needing a push; workflow ready |
| Preserve Debug/Release/sanitizer configs | **DONE** | W12 — UBSan added to CI |
| **Do not introduce undefined behaviour** | **DONE** | W12 — 5 pre-existing UB findings, all fixed; UBSan clean |
| Do not suppress warnings / disable tests | **held** | zero warnings suppressed; zero tests weakened; 3 reverts instead |
| Validation: build, unit, integration, e2e, benchmarks | **done** | §7 |
| Test coverage for the code being refactored | **done** | W2 — operators had **zero** unit tests; 82 added, 772 → 854 |
| Confirm symbol visibility and linkage | **done** | `nm` diff, gate 3 |

---

## 2. Baseline

| | |
|---|---|
| `src/` | 49,618 lines, 323 files (201 headers, 21,420 lines) |
| `test/` | 17,875 lines — 772 unit tests, 98/101/103 e2e (normal / small-vector / durable) |
| Builds | Release, Debug+ASan, Debug+TSan, small-vector |
| Benchmarks | OLAP (TPC-H + ClickBench, 26 q) vs DuckDB; DML (12) vs SQLite |

**Already healthy — do not spend effort here.** No owning raw `new`/`delete` pairs (all 37 `new`
expressions are immediately owned; every buffer member is `unique_ptr<T[]>`; no leak on the throwing
path in `BufferedFileWriter`'s constructor). No `goto`, no `malloc`/`free`, 7 benign C-style casts,
no `thread_local`. Almost no *textual* copy-paste — a clone scan found 6 cross-file groups, all
operator-header override declarations.

**Completed:** anonymous namespaces 98→3 (`nm`: 6,884→7,251 exported, 0 removed); 13 × `TypesOf` →
`Schema::GetTypes()`; 6 × `Resolve*` → `ResolveTableStorage<T>`; a real ODR hazard (two *different*
`TreeFixture` structs); the aggregate cleanup (30 vectorized loops before and after); `main()`
156→16 lines (20 CLI invocations diffed byte-for-byte); planner walkers 3→1 (52 EXPLAIN plans
diffed); a Release-only failing death test gated on `NDEBUG`.

**Then, working the plan in order:** W1 (tooling + CI), W12 (UBSan + 5 UB fixes), W10 (dead code),
W2 (operator tests — which found and fixed a decimal-width memory-safety bug). Detail in §5.

**Running totals (final):** 136 files changed, +2,101 / −2,723 lines (net **−622**), 20 new files
(`.clang-format`, `.clang-tidy`, `.github/workflows/ci.yml`, 3 source headers incl.
`physical_type_dispatch.h`, the operator-test harness, 9 operator test files, an `.slt` + parquet
fixture for the decimal-width guard). Unit tests **772 → 854**; e2e **98/101/103 → 99/102/104**.
Anonymous namespaces **98 → 3**. `case PhysicalType::` labels **413 → 302**. TODO markers
**17 → 9**. UBSan findings **5 → 0**. Macros removed: 4 (3 DISALLOW_* + dead MappingType).

---

## 3. Duplication, measured

| Dispatch family | Case labels | Files |
|---|---|---|
| `case PhysicalType::` | **413** | 18 |
| `case LogicalTypeId::` | **294** | 12 |
| `case ExpressionType::` | 34 | 3 |
| `case PlanType::` | 22 | 3 |
| **Total** | **763** | |

Plus 52 `dynamic_cast<const Bound…&>` in binder/planner, 12 operators repeating the
sink/source-state pattern, 34 `Normalify`-then-`GetData` pairs.

This is *structural* duplication — same shape, different identifiers — which is precisely what a
textual clone detector cannot see. The low clone count above is misleading, not reassuring.
Handled by **W4**.

---

## 4. Simplification catalogue

Concrete, named, measured targets. Each names the technique, so these are executable rather than
aspirational.

### S1 — Parameter-heavy functions: **62 with ≥7 parameters**

| Function | Params | Location |
|---|---|---|
| `RowOperations::Match` / `MatchColumn` | **12** | `row_operations.{h,cpp}` |
| `EmitBatch` | 12 | `physical_hash_join.cpp:267` |
| `TemplatedMatchCol` / `TemplatedMatchStringCol` | 11 | `row_operations.cpp:89,120` |
| `SelectGenericLoop` / `SelectGenericLoopSelSwitch` | 11 | `binary_execution.h:270,301` |
| `StageMatches` | 11 | `physical_grace_hash_join.cpp:129` |
| `SelectFlatLoop` / `SelectFlatSelSwitch` | 9 | `binary_execution.h:201,231` |
| `UpdateByPhysicalType` | 9 | `aggregate_update_kernels.cpp:171` |
| `ApplyMvccModify` | 8 | `mvcc.cpp:40` |

The `Match` family is the worst: **12 parameters threaded unchanged through 4 call layers**.

*Technique:* a `struct MatchContext` passed by `const&`, grouping the invariant arguments
(`rows`, `layout`, `col_offset`, `col_no`) and leaving the varying ones explicit.
*Caution:* these are hot kernels. A parameter object must be a plain aggregate the compiler can
scalarise — verify with the codegen gate, and revert if register allocation degrades.

### S2 — Deep nesting

| File | Max depth |
|---|---|
| ~~`bind_create.cpp:219`~~ | ~~10~~ → 5 ✅ (W3) |
| `physical_table_scan.cpp:131` | 9 |
| `parquet_writer.cpp:305` | 8 |
| `physical_nested_loop_join.cpp:147` | 8 |
| 8 further files | 7 |

*Technique:* guard clauses and early `continue`/`return`; extract the innermost block. Depth 10 in
`bind_create` is a binder path with no perf risk — start there.

### S3 — Flag parameters: 30 `bool` parameters in public headers

`ReleaseDependents(Pipeline&, bool no_output_possible)`, `AddEdge(…, bool is_equi, …)`,
`Match(…, bool null_equal)`, `ApplyMvccModify(…, bool is_delete, …)`, `SetNull(Vector&, bool)`,
`PRLHashTable(…, bool null_equal_keys, …)`.

*Technique:* where the flag is a compile-time constant at every call site (`null_equal`,
`is_delete`), make it a template parameter — this **removes a branch from a hot loop** as well as
clarifying the call. Where it is genuinely dynamic, a two-value `enum class` names the argument at
the call site. Do not blanket-convert.

### S4 — Out-parameters: ~50 non-const ref/pointer outputs

*Technique:* return by value where the type is cheap or NRVO applies. **Do not** convert the
`DataChunk`/`Vector` out-params — those exist specifically to reuse caller-owned buffers, and
returning would allocate per chunk. This item is mostly *documenting* which out-params are
deliberate.

### S5 — Large classes

| Class | Members |
|---|---|
| `PRLHashTable` | **21** |
| `ColumnReader` | 17 |
| `TopNHeap` | 15 |
| `Value` | 14 |
| `Executor` / `BumbleBeeInstance` | 13 |

*Technique:* group cohesive member clusters into nested structs (e.g. `PRLHashTable`'s probe state
vs. directory state vs. layout). `Value` is a tagged union by design — leave it. This is the one
place where composition-over-inheritance from the brief applies concretely.

### S6 — Indirect calls: 13 `std::function`

*Technique:* check whether any sit on a per-row or per-chunk path; replace those with a template
parameter or `function_ref`-style non-owning callable. Leave the setup-path ones alone.

### Closing dispositions (end of refactor)

- **S1 (parameter-heavy):** the `Match` family's 12 parameters survive — the `MatchContext`
  aggregate was pre-emptively ruled out by the Copy experiment (W3): any reshaping of these
  kernels' calling convention reshuffles register allocation, and S1's own caution says revert.
  W4's conversion of `MatchColumn` kept every argument explicit for exactly this reason. The
  parameter count is the cost of a calling convention the codegen gate froze.
- **S2 (deep nesting):** worst offender done (`bind_create` 10 → 5, W3); `parquet_writer` fell
  with the Flush split (W3); `physical_nested_loop_join` is the W3-reviewed state machine;
  `physical_table_scan` (9) and the depth-7 tail remain as recorded targets — all in operator
  code where the W2 tests now stand guard for whoever takes them.
- **S3 (flag parameters):** the hot ones are already template parameters (`NULL_EQUAL` in the
  Match family, `HAS_RSEL` in the hash kernels, `HAS_NULLS/HAS_SEL` in the aggregate kernels) —
  the survey confirmed the codebase had adopted the technique where it pays. The genuinely
  dynamic remainder (`is_delete`, `no_output_possible`) are two-value calls at cold sites.
- **S4 (out-params):** documented as designed — `DataChunk`/`Vector` out-params reuse
  caller-owned buffers; converted nothing (per plan).
- **S5 (large classes):** not restructured. `PRLHashTable` (21 members) is the join/aggregate
  hot core; member regrouping is a layout change with only the drift-dominated A/B to defend it
  — same verdict as false-sharing padding (W9). `Value` stays a tagged union by design.
- **S6 (`std::function`):** audited — the `unary_execution.h` occurrences are *deduced-template
  defaults* (callers instantiate with their lambda's real type; no erasure), and every true
  `std::function` sits on plan-time or per-task paths. No per-row type erasure exists.

### S7 — Already-executed simplifications, as the pattern to follow

`DispatchNullsAndSelection` (12-way ternary → 3 calls, same instantiations), `Schema::GetTypes()`
(13 copies → 1), `ResolveTableStorage<T>` (6 → 1), `VisitChildren` (3 tree walks → 1),
`aggregate_semantics.h` (2 rules stated once instead of 8 times), `main()` 156 → 16 lines.

---

## 5. Workstreams

### W1 — Enforce the conventions the project already documents — **✅ DONE**
`DEVELOP.MD` claimed trailing return types were "enforced by `modernize-use-trailing-return-type`"
and "`modernize-*` checks are on". **Neither was true** — no `.clang-tidy`, no `.clang-format`, no
CI. Every convention was enforced by reviewer memory, which is plausibly how 763 duplicated case
labels accumulated.

**✅ `.clang-format`** — `BasedOnStyle: Google, ColumnLimit: 120, SortIncludes: false`.
- 120 was *derived*, not guessed: sweeping 100/105/110/115/120/125 gave 9571 / 5670 / 3690 / 2284 /
  **1516** / 2282 changed lines. 120 is the provable minimum, confirming the intended width.
- `SortIncludes: false` because `DEVELOP.MD` requires include order to stand as written.
- **Applied to the 84 files touched in this refactor** (now 0 residual diff). This fixed a defect
  *this refactor introduced*: the de-anonymisation script prepended `static ` to signatures without
  re-indenting their wrapped parameter lines, leaving ~812 lines misaligned by 7 columns.
- Verified behaviour-neutral: exported symbols unchanged (the single delta is `fmt`'s
  `AggregationType` formatter, from the earlier aggregate change), 772/772 unit, 98 e2e.
- **~704 lines of pre-existing drift remain in untouched files.** Deliberately not reformatted —
  a tree-wide sweep belongs in its own commit, not mixed into a refactor. CI therefore gates on
  *changed lines* (`git-clang-format --diff`), the standard way to introduce a formatter.

**✅ `.clang-tidy`** — `bugprone-* modernize-* performance-* readability-*` minus 9 checks, each
with its reason recorded in the file. Config validated (`--verify-config`). Notable: **every
non-lambda function already satisfies `modernize-use-trailing-return-type`** — all 11 sample hits
were lambdas, where the check cannot be excluded and the leading-`auto` form is clearer. So the
documented convention was real; only its enforcement was missing.

**✅ CI** (`.github/workflows/ci.yml`) — the matrix that until now existed only as hand-run
commands: Release / ASan / TSan / **UBSan** / small-vector builds, unit tests, e2e in all three
modes, plus format and tidy jobs scoped to changed files.

*Caveat:* clang-tidy could not be validated locally — Homebrew LLVM 21 cannot parse Apple's SDK
headers (`missing '(' after '__has_feature'`). It is wired up **advisory** so the first CI run
reports rather than blocks; flip `continue-on-error` off once green on the Linux toolchain.

### W2 — Unit-test the physical operators — **✅ DONE (harness + 82 tests)**

There were **zero** unit tests for `execution/operator/{aggregate,join,order,persistent,scan}`,
`execution/sort`, `execution/aggregate`, `storage/mvcc` — all reached only through SQL.

**✅ Harness** (`test/unit/include/operator_test_util.h`): `RowScan` (replays literal rows at a
caller-chosen `chunk_size`), `RowCollector` (materialises output as `Value`s), `OperatorHarness`
(catalog + txn manager + client context + `Run()`). Making `chunk_size` a parameter means the
multi-chunk path — previously only reachable via the small-vector build — is testable directly.
Extended for the out-of-core operators with a `MemoryDiskManager` + `BufferPoolManager` (spill
backing for `SpillCollection`); shrinking `client.mem_.SetBudget()` forces the spill paths. The
catalog is bpm-backed too, so `CreateTable()` builds a real `TableHeap` — the write operators run
against real storage and real transactions, not mocks.

**✅ 82 tests, suite 772 → 854:**
- `physical_sort_test.cpp` (11): ASC/DESC, the fixed NULLs-last/first convention, chunking
  independence, empty input, duplicate keys; TopN limit > input, limit 0, chunking independence,
  NULL keys not crowding out real values.
- `physical_hash_join_test.cpp` (10): inner match, **empty left / empty right on both INNER and
  LEFT**, LEFT NULL-padding, `NULL != NULL` never joining, many-to-many cartesian product,
  chunking independence.
- `physical_hash_aggregate_test.cpp` (10): COUNT/SUM/MIN/MAX/AVG, empty input producing *no*
  groups, **NULL as its own GROUP BY group** (the opposite of join equality — the group table is
  built with `null_equal_keys = true` for exactly this), NULL arguments skipped, an all-NULL group
  yielding count 0 and NULL values, **string MIN/MAX** including the empty string and a 200-byte
  value that forces the off-row string store, chunking independence, and 500 distinct groups to
  reach the partitioned sink and partition-wise merge rather than the single-table fast path.

**Two things this immediately surfaced — both in *my* code, which is the point:**
1. `RowCollector` initially omitted `BuildPipelines`, so it fell through to the streaming default
   and left `Pipeline::sink_` null → SEGV. ASan pinpointed it in one run.
2. My LEFT JOIN tests asserted the wrong side. `PhysicalHashJoin`'s last two constructor
   parameters are *named* `build, probe`, but they are child 0 = **left** and child 1 = **right**;
   `BuildChildIdx()` returns `PreservesLeft() ? 1 : 0`, so for LEFT/SEMI/ANTI the *second*
   argument is hashed and the first streams as the probe. **The parameter names contradict the
   semantics** — a naming fix for W3, and exactly the kind of contract that was undocumented
   because nothing tested the operator directly.

- `physical_nested_loop_join_test.cpp` (11): equi and **non-equi** (`<`) predicates, the
  constant-TRUE cross product, empty outer/inner on INNER and LEFT, LEFT NULL-padding — including
  **padding exactly once when the inner side spans many chunks** (the decision is only final after
  the *last* inner chunk, the state the re-entrant match cursor has to get right), NULL keys never
  satisfying the predicate, many-to-many, chunking independence. Note the operator's shape is the
  hash join's mirror image: the *outer* (left) side streams and the *inner* (right) materialises.
- `physical_grace_hash_join_test.cpp` (13): the same assertions as the in-memory hash join test on
  purpose — the operator's contract is "identical rows" — plus the paths only this operator has:
  500 rows spread across many spill partitions stitched back together, **budget 1 byte** forcing
  the phase-3 oversized-pair repartition, a **single hot key under budget 1** forcing the
  block-nested-loop fallback (100×100 = 10,000 rows), and LEFT's dedicated NULL-key probe spill.
- `physical_external_merge_sort_test.cpp` (7): the PhysicalSort contract (ASC/DESC, NULLs
  last-in-ASC/first-in-DESC) surviving the spill round trip, **budget 0 forcing a run per input
  chunk** so the k-way merge does the work (8 runs), 200-byte strings re-homed through spill pages
  (ASan is the real assertion), duplicates each landing in their own run, chunking × budget
  independence, empty input.

- `physical_dml_test.cpp` (8): insert/update/delete against a real bpm-backed heap in real
  committed transactions. Count-row reporting, chunking independence, empty input; UPDATE both as
  constant fill and as **v := k recomputed from the pre-image**, with the untouched column
  preserved; and two MVCC contracts at operator level: **a snapshot begun before a delete commits
  still sees the rows**, and **first-committer-wins rejects the second writer** of the same row
  (surfaced either as an execution-time exception or a failed commit — the test accepts both,
  pinning the outcome without over-specifying the mechanism).
- `physical_table_scan_test.cpp` (5): full scan, empty table, **the column-pruning contract**
  (projection `{1}` still emits a full-width chunk with the pruned column constant-NULL — no
  renumbering), `emit_rids` appending a distinct non-NULL BIGINT per row (what Update/Delete
  address writes with), and 5,000 rows spanning many heap pages so morsel claiming does real work.

- `physical_parquet_scan_test.cpp` (7): full scan of one part file, several part files stitched
  from (file, row group) morsels, empty manifest, the pruning contract (undecoded columns
  constant-NULL in a full-width chunk), `emit_rids` yielding exactly
  `(file_idx << 32) | row_in_file`, and the two decimal-width cases below. The write path is
  driven the way the real writers commit: `WritePartFile` + an atomically named manifest.

**A third real bug, found by writing the decimal-width test.** The parquet column reader decodes
into the backing integer selected by the **file's** decimal width (`DeriveLogicalType` normalises
INT32 storage to DECIMAL(9,s), INT64 to DECIMAL(18,s)), while the scan's output vector is
allocated with the **table's declared** width — and `ExternalSchemaMatches` compared DECIMALs by
`LogicalTypeId` only, so the mismatch sailed through both CREATE and scan open. An INT32-stored
file read through a DECIMAL(18,2) declaration type-punned 24 int32 payloads into an int64 buffer
(observed: 12 packed garbage values + 12 zeros); the mirror case — a *narrower* declaration —
**heap-overflows** once a full 1024-row chunk of int32 payloads lands in an int16-backed vector
(4,096 bytes into a 2,048-byte buffer). Fixed where the docstring already promised it ("fail
loudly instead of decoding garbage"): `ExternalSchemaMatches` now also requires the same physical
backing for DECIMAL, so the declaration is rejected at CREATE / scan open. Same-backing widths
(5–9 over INT32, 10–18 over INT64) still adopt the file exactly as before; the only behaviour
change is garbage/UB → error. Covered by 2 unit tests (mismatch rejected; matching-backing
DECIMAL(9,2) round-trips the externally-written `int32_decimal.parquet` fixture — BumbleBee's own
writer emits decimals as DOUBLE, so this layout only comes from external files) and a new
`external/decimal_declared_width.slt` (e2e corpus 98/101/103 → 99/102/104).

This also closes W12's *"still open"* note: the within-file zero-fill-vs-sign-extend gap is
unreachable for INT32/INT64 storage (the reader's width is derived *from* the storage, so they
cannot disagree), and the cross-schema variant is now rejected by this guard. The W12 clamp
remains as defence in depth.

*Verified:* 854/854 Release · 854/854 ASan · UBSan clean (854) · TSan clean on concurrency + all
82 operator tests · e2e 99 + 102 smallvec + 104 durable.

### W3 — Split functions too long to reason about — **✅ DONE (8 split, 4 hot-path reviewed with verdicts, 1 exempted)**

**The four † hot-path functions — resolved by experiment, not assertion.**

- **`VectorOperations::Copy` 187† — split attempted, failed the codegen gate, reverted.** The
  natural extraction (STRING/LIST/ARRAY payload cases + the validity mirror as same-TU statics)
  was tried both ways and measured against the baseline object file (compilation determinism
  verified first — an untouched rebuild is byte-identical):
  1. Plain statics: clang outlined the three payload helpers (3 new internal symbols, +72
     disassembly lines) — one extra call per chunk-level Copy on those paths.
  2. `[[gnu::always_inline]]`: everything inlined back, but register allocation reshuffled —
     **796 of ~1,450 instructions differ, stack frame 0x190 → 0x1a0, net +10 instructions.**
  That is precisely the degradation S1's caution names ("revert if register allocation
  degrades"), and an A/B on this drifting machine could not prove a delta this small either way.
  Reverted per the rollback rule; the restored file recompiles **byte-identical** to the baseline
  object, so the revert is proven complete.
- **`ExpressionExecutor::Evaluate` 133†** — reviewed, kept. The length is enumeration: ten
  expression types, most branches 3–8 lines calling the vectorized `Run*` kernels (the per-row
  work lives there, not here). The two longer branches are the strict-CAST error path and the
  prepared-IN fast path, whose bulk is a flat type dispatch — the exempted shape. Extraction
  would trade a demonstrated codegen risk for zero comprehension gain.
- **`PhysicalNestedLoopJoin::Execute` 111†** — reviewed, kept. A re-entrant state machine
  (`processing_` / `pairs_ready_` / `pair_cursor_` / `left_done_`) whose three return points and
  mid-loop `continue` ARE the logic; extracting phases would scatter the state transitions behind
  call boundaries and out-of-band result codes. Its contract is now pinned by the 11 direct unit
  tests W2 added (including padding-once-across-many-inner-chunks, the exact re-entrancy case).
- **`Vector::Normalify` 105†** — reviewed, kept. An encoding switch with early returns plus a
  flat one-liner-per-type dispatch; the only multi-line block (the nested-constant aliasing fix)
  is 14 commented lines. Same enumeration verdict.

The brief's own escape hatch applies: a change that cannot be *shown* to preserve performance is
not forced. One split was tried, measured, and reverted; the other three verdicts rest on that
measurement plus shape analysis, and all four remain under the full test matrix.

*W3 closing verification (post-revert tree):* 854/854 Release · 854/854 ASan · UBSan clean ·
TSan clean (concurrency + all operator tests) · e2e 99 + 102 smallvec + 104 durable ·
`vector_copy.cpp.o` byte-identical to its pre-experiment baseline.

**✅ `ParquetWriter::Flush` 224 → 70 lines.** The per-column body split along its three phases,
all file-static, all verbatim moves: `WriteDefinitionLevels` (the RLE bit-packed validity run),
`WritePlainColumnPayload` (the per-type PLAIN-encoding switch), `CompressPage` (the codec switch;
the caller-owned holder keeps the UNCOMPRESSED case aliasing the page buffer with no copy, exactly
as before). What stays in `Flush` is the page/row-group bookkeeping that reads writer state.
*Gate:* **written-file byte identity** — an 8-type × 3-row (incl. all-NULL row) external-table
insert hashes to the same SHA before and after (write determinism itself verified by double-run
first); 854/854 Release + ASan · UBSan clean · e2e 99 + 102 + 104 · changed-line clang-format
clean. The byte gate exercises the UNCOMPRESSED codec (the write path's default); the moved
SNAPPY/GZIP/ZSTD arms are verbatim and covered functionally by the writer round-trip unit tests.

**Finding while building that gate (pre-existing, not fixed):** writing a DECIMAL column to an
external table produces a file the table then refuses to scan — the writer stores decimals as
physical DOUBLE ("until FIXED_LEN_BYTE_ARRAY decimals are implemented"), so the re-read derives
DOUBLE and the schema check (TypeId equality, which predates this refactor) rejects it. INSERT
succeeds; every later SELECT fails. Fixing it means either writing real decimals or relaxing the
check for the writer's own DOUBLE encoding — a product decision, not a refactor. Also noted:
DECIMAL/FLOAT literals don't coerce in VALUES (needs explicit CAST), which is why no e2e ever hit
this.

**✅ `OptimizeJoinOrder` 238 → 111 lines.** The catalogue's longest function, split along its own
phase comments: `CollectRegionLeaves` (the region walk, previously a recursive `std::function`
lambda), `ClassifyRegionConjuncts` (single-table filters vs join edges; returns false to abandon
the region on an unexpected conjunct shape), `AddEdgesWithKeyNdv` (the union-find NDV equivalence
classes), and `EmitJoinTree` (tree → plan nodes, previously a 52-line recursive lambda; now a
plain recursive function, which also drops the `std::function` indirection). The `PendingEdge`
struct moved from function-local to file scope. Graph construction and the 2-leaf/N-leaf tree
choice stayed inline — the tree choice reads `join_order_` (a member), and extracting it would
mean a 5-parameter helper, trading one smell for another. One API tweak: `IsPredicateTrue` became
`static` (public) on `Optimizer` — it reads no state, and the file-local region walk needs it.
*Gate:* a second, join-order EXPLAIN battery — 4 tables at 2048/256/32/4 rows (real catalog
cardinalities, built by self-insert doubling), 8 queries covering build-side swap kept and
refused, 3- and 4-way chains, local filters, a non-equi edge and a disconnected region —
**byte-identical**, plus the subquery battery re-checked identical; 854/854 Release + ASan ·
UBSan clean · e2e 99 + 102 + 104 · changed-line clang-format clean. Benchmarks not rerun,
justified: plan choice is proven unchanged by the EXPLAIN gates and the pass runs at plan time.

**✅ `PruneColumns` 152 → ~100 lines.** Two extractions: `SplitRequirementAtSeam` (the left/right
requirement split, previously duplicated verbatim between the HashJoin and NestedLoopJoin cases —
a real DRY win, not just a move) and `PruneHashJoin` (the one 45-line case in an otherwise
uniform recursive switch; carries the build-side live-column annotation logic and its
convention-mirroring comment with it). The switch now reads as one rule per plan-node type.
*Gate:* EXPLAIN battery byte-identical (every plan in it shows `columns=[...]` on its scans);
NLJ pruning spot-checked (`a JOIN b ON a.x < b.p` keeps `[x,y]` / `[p]`); 854/854 Release + ASan ·
UBSan clean · e2e 99 + 102 + 104 · changed-line clang-format clean.

**✅ `PlanExpression` 102 → 68 lines.** The function is a flat dispatch switch — a fine shape —
whose length came from one 36-line SUBQUERY case among ten short ones. That case became
`PlanScalarSubqueryExpression`; the other cases stayed put, because extracting a 3-line case just
moves it. The `resolved_subqueries_` lookup keys on the bound expression's address (the
plan-pointer identity the subquery machinery depends on) — passing the derived reference
preserves the address, verified by the correlated-scalar e2e queries.
*Gate:* same EXPLAIN battery byte-identical · 854/854 Release + ASan · UBSan clean ·
e2e 99 + 102 + 104 · changed-line clang-format clean.

**✅ `PlanSubqueryPredicate` 110 → 34 lines, `PlanCorrelatedScalarSubquery` 107 → 61 lines.**
Each was a sequence of commented phases; the phases became named functions. The two EXISTS
branches (`PlanCorrelatedExists`, `PlanUncorrelatedExists`) are member helpers — they need
planner state but not `CorrelationPair`, which stays file-local. The scalar rewrite
(`RewriteScalarSubqueryForDecorrelation`) and the collision-avoiding rename projection
(`RenameSubqueryColumns`) are file-static — pure statement/plan surgery, no planner state. The
IN-subquery branch stayed inline: 14 lines, and extracting it would just move the comment.
*Gate:* a 12-query EXPLAIN battery (IN / NOT IN / correlated + uncorrelated EXISTS / correlated
scalar with and without the key-source restriction / nested IN) — **byte-identical before and
after**; 854/854 Release + ASan, UBSan clean, e2e 99 + 102 + 104, changed-line clang-format
clean. TSan skipped with justification: planning runs single-threaded on the client thread.

**✅ `BindCreate` 131 → 47 lines, max nesting depth 10 → 5** — the S2 catalogue's worst offender
and the recommended starting point (binder path, no perf risk). Extracted four file-static
helpers, each preserving its fragment byte-for-byte: `BindStorageOptions` (the WITH list),
`SetPrimaryKey` (the two-PK error, previously duplicated in both constraint paths),
`BindColumnConstraints`, `BindTableConstraints`. One oddity deliberately preserved and now
documented at its extraction site: the table-level constraint case iterates from its cell over
the *remaining* tableElts cells — reshaping that would be a behaviour change, not a refactor.
*Verified:* 854/854 Release + ASan · UBSan clean · e2e 99 + 102 + 104 · clang-format clean.

13 exceeded 100 lines: ~~`OptimizeJoinOrder` 238~~ ✅, ~~`ParquetWriter::Flush` 224~~ ✅,
`VectorOperations::Copy` 187† (split reverted — failed the codegen gate, see above),
~~`PruneColumns` 152~~ ✅, `ExpressionExecutor::Evaluate` 133† (kept — enumeration, see above),
~~`BindCreate` 131~~ ✅, `PhysicalNestedLoopJoin::Execute` 111† (kept — state machine, see above),
~~`PlanSubqueryPredicate` 110~~ ✅, ~~`PlanCorrelatedScalarSubquery` 107~~ ✅,
`Vector::Normalify` 105† (kept — enumeration, see above), ~~`PlanExpression` 102~~ ✅.
(† hot path.) `node_tag_to_string.cpp`'s 814-line enum→string table stays.
*Gate:* EXPLAIN diff (planner/binder); codegen + A/B (hot path). The subquery EXPLAIN battery is
three small tables (orders/customers/items) and one EXPLAIN per shape listed in the entry above —
cheap to rebuild for the next planner split.

**✅ Naming defect found by W2 — fixed.** `PhysicalHashJoin`'s last two constructor parameters
were named `build, probe`, but they are child 0 = **left** and child 1 = **right**.
`BuildChildIdx()` returns `PreservesLeft() ? 1 : 0`, so for LEFT, SEMI and ANTI — three of the
five join types — the parameter called `build` was in fact the probe side. It cost a wrong test
assertion before reading the operator. Renamed to `left, right` with a constructor comment saying
*why* they are not named build/probe; `PhysicalGraceHashJoin` already named them `left, right`.
Names only — call sites pass positionally and parameter names don't mangle, so no symbol or
behaviour change (854/854 unit, e2e 99).

### W4 — The 763 dispatch case labels — **✅ DONE (the convertible half converted; the rest recorded)**

**The primitive:** `src/include/type/physical_type_dispatch.h` — `DispatchNumericPhysicalType`
(the ten numeric types) and `DispatchNumericAndStringPhysicalType` (+ `string_t`), each taking a
generic lambda and an `otherwise` callable so every site keeps its own fallback and error message
with single-switch codegen. `[[gnu::always_inline]]`, learned the hard way: without it clang
emits the template as a weak external symbol instead of folding it into the call site. Second
recipe ingredient: hoist `GetLogicalType().GetPhysicalType()` into a local before the call, or
the `LogicalType` temporary lives across the dispatch and drags a destructor onto the unwind path.

**Converted — 6 files, 122 `case PhysicalType::` labels → 6 dispatcher calls (413 → 302 labels
tree-wide), each gated on its object file:**
| File | Sites | Codegen verdict |
|---|---|---|
| `aggregate_update_kernels.cpp` | UpdateByPhysicalType | **byte-identical** |
| `create_sort_key.cpp` | ConstructSortKey | **byte-identical** |
| `vector_generators.cpp` | SwitchGenerateSequence | **byte-identical** |
| `vector_comparison.cpp` | equal-type Select | size identical, **0 of 796 functions changed** (layout order only) |
| `vector_hash.cpp` | Hash + CombineHash switches | symbols =, size −0.8%, frames =, +5 instr in each CombineHash's cold LIST/throw tail; kernels intact (Hash byte-equal) |
| `row_operations.cpp` | Match/ScatterKeys/Scatter/FullScan/Gather (54 labels) | symbols =, size +0.38%, frames =, 2 of 22 fns changed (+1, +11 — the string fallback leaving the jump table) |

**Attempted and REVERTED (restored byte-identical), the boundary discovered:** header
inline-member switches do not convert cleanly. `generic_key.h`'s `GenericComparator::operator()`
grew 120 → 129 instructions — the B+tree's hottest comparator; `aggregate_state.h`'s
`UpdateVector` left the lambdas **outlined** (11 new symbols, +4.75% object). The rule that
emerged: **file-local static dispatch shells convert byte-identically or near it; switches
embedded in header member functions perturb inlining and fail the gate.**

**A/B benchmark (OLAP, interleaved, 8 runs):** the first batch (before-first order) read +19%
slower after; the reversed batch (after-first order) read **−33% faster** after, winning 20-0-6
per query. The sign flips with run order and the within-arm spread dwarfs both (clickbench q32's
own baseline moved 7 s → 28 s between batches) — per §7's standard the honest conclusion is **no
measurable change**, corroborated by the per-object proof that every kernel loop is
instruction-identical and only cold dispatch tails moved by single-digit instruction counts.

**Not converted, and why (the census's other families):**
- `vector_copy.cpp` (14), `expression_executor.cpp` (22): switches embedded in the W3-reviewed
  hot functions (`Copy`, `Evaluate`) — the codegen-gate verdicts there govern.
- `top_n_heap.cpp` (24): per-case sign handling (`^ SIGN` casts, signed/unsigned template flags)
  — non-uniform by construction.
- `vector_arith.cpp` (25): Negate is a 6-type subset; the equal-type path dispatches on
  `LogicalTypeId` folding DATE/TIMESTAMP into the integer cases — the fold the plan predicted.
- `vector_cast.cpp` (19): the operator template varies per case (`TryIntegerCast` vs
  `TryDoubleCast` vs string/date routes) — a type-traits layer would be new machinery, not DRY.
- `logical_type.cpp` (46), `value.cpp`/`value.h` (24): flat metadata tables and the per-row Value
  boundary — the `node_tag_to_string` exemption shape.
- `vector.cpp` (55): Normalify (W3 verdict) + Value-boundary Get/SetValue switches.
- The `LogicalTypeId` (294), `ExpressionType` (34), `PlanType` (22) families: binder/planner
  walkers, coercion rules and node factories — every case builds a *different* thing; there is no
  common `F<T>` to dispatch to. These are enumerations, not duplication.
*Gate results:* per file above · A/B "no measurable change" (both orders run) · 854/854 Release +
ASan · UBSan clean · TSan clean · e2e 99 + 102 + 104 · changed-line clang-format clean.

### W5 — Const-correctness and API hygiene — **✅ DONE (local scope; `explicit` sweep delegated to CI tidy)**

- **✅ `make_unique_for_overwrite`** on every raw array-new site (12 conversions across 9 files,
  incl. the BufferedSerializer grow path, now exception-safe end to end). The trap was respected:
  `_for_overwrite` keeps default-init — plain `make_unique<T[]>` would have silently zeroed every
  vector/string-heap/compression buffer on allocation.
- **✅ `[[nodiscard]]` on all 9 `Try*` APIs** (`TryGetColIdx`, `TryLockForWrite`,
  `TryOptimistic{Insert,Delete}`, `TryFromDate`, `TryConvert{Date,Timestamp}`, `TryCast`,
  `TryReserve`). The warning sweep this triggered found **8 discarding call sites, every one
  intentional** (the merge sort's commented best-effort reserve, non-strict `Cast`'s
  failures-are-NULL contract, and test probes) — each now carries an explicit `(void)`, so the
  build is warning-clean and any future silent discard is a diagnostic.
- **✅ `const_cast` audit (7 sites):** all are const-view-into-nonconst-API shims (`string_t` is a
  view type by design; the RLE decoder wraps a read-only buffer; `ZERO_VECTOR` is never written) —
  none mutates a genuinely-const object, so none is UB. No change.
- **✅ `noexcept` audit (19 sites):** exclusively on move constructors/assignments, where it is
  load-bearing (`move_if_noexcept`). Genuinely non-throwing (pointer/handle swaps). No change.
- **`explicit` (72 candidates):** the mechanical sweep needs `google-explicit-constructor`, which
  cannot run locally (the W1 toolchain caveat: Homebrew clang-tidy chokes on Apple's SDK). The
  check is now **added to `.clang-tidy`** so the Linux CI tidy leg reports it; the known-intended
  implicit conversions (`SelectionVector(sel_ptr_t)` et al.) already carry NOLINTs.

### W6 — Header hygiene — **✅ CLOSED: dropped by its own gate, candidate recorded**

The gate was "clean-build wall-clock — reject if not measurably faster." Measured first: two
**identical** clean builds of `bumblebee_lib` on this machine took **304 s and 399 s — a 31 %
A/A spread** (the same instability the benchmark protocol documents). A header change's plausible
effect (single-digit percent) cannot be measured under that noise, so per the gate no change
ships. The candidate, recorded for a quieter machine or CI: both `common/exception.h`
(88 includers) and `common/macros.h` (61) pull `<iostream>` — and its per-TU `ios_base::Init`
static — for one debug-only `std::cerr` in the `Exception` constructor and one in
`BUMBLEBEE_ENSURE`. Mechanics: move the exception print out of line into `exception.cpp`;
switch `ENSURE` to `<cstdio>` (`fprintf(stderr, ...)` — all 20 messages are string literals).
`common/config.h` is already lean (`<chrono>/<cstddef>/<cstdint>`).

### W7 — Ownership: 110 `shared_ptr` — **✅ DONE (the buffer-pool trio converted; the shared ones verified shared)**

**✅ The buffer pool's `latch_` / `replacer_` / `disk_scheduler_` are now single-owner** —
`std::mutex` by value, the other two `unique_ptr` — and the page guards hold **non-owning raw
pointers** with the lifetime argument written at the member declaration: a live guard pins one of
the pool's frames, so a guard outliving the pool was already a bug; the shared_ptr copies bought
no safety and cost **three atomic refcount pairs on every page access** (guards are constructed
per page fetch — this was the hottest copy path in the ownership census). Only two files touched;
`FrameHeader` stays shared (a frame is genuinely co-owned by the pool's frame table and whichever
guard holds it during eviction decisions).
*Gate:* **the full 854-test suite under TSan — clean** (first-ever full-suite TSan run; before
this only the concurrency filter ran) · 854/854 Release · e2e 99.

**Verified genuinely shared, kept:** `TableInfo` (catalog handout, `NULL_TABLE_INFO` sentinel is
API shape), `IndexInfo` (same pattern), `ByteBuffer`/`ResizeableBuffer` (parquet reader state
outliving operator states — the CLAUDE.md-documented reason `GlobalParquetAllocator` exists),
`ParallelScanState` (shared by scan tasks), `Transaction` (manager map + client), `FrameHeader`
(above), `sel_ptr_t`/validity buffers (zero-copy slicing shares them by design).

### W8 — Macros and standard-library preference — **✅ DONE**

- **✅ `DISALLOW_COPY*` → explicit `= delete` members** at all 11 use sites (Database, the
  binder/planner ContextGuards, TableStorage, Index, DiskManager, DiskScheduler,
  BufferPoolManager, ArcReplacer, Transaction); the three macro definitions removed from
  `common/macros.h` along with their per-line NOLINTs (which existed only to appease tidy about
  the macro style).
- **✅ `MappingType` deleted** — it was a dead global macro leak (zero uses), squatting on an
  identifier for every includer.
- **KEPT, each with a reason the survey turned up:**
  - `INDEX_TEMPLATE_ARGUMENTS` / `BPLUSTREE_TYPE` / `INDEXITERATOR_TYPE` /
    `B_PLUS_TREE_*_PAGE_TYPE`: C++ has **no alias spelling** for a template head, and out-of-line
    member definitions must name the true class (an alias template in that position is
    ill-formed) — expanding is 90+ lines of churn in bustub-lineage files for zero safety.
  - `LEAF/INTERNAL_PAGE_SLOT_CNT`: these expand **in the includer's template context** —
    `b_plus_tree.h`'s constructor defaults compute internal-page geometry with *its own*
    ValueType. A class-scope `static constexpr` would compute a different default and change the
    on-disk tree geometry, which is frozen. Documented, not touched.
  - `BUMBLEBEE_ASSERT`/`ENSURE`/`UNIMPLEMENTED`/`UNREACHABLE`, `LOG_LEVEL_*`, `BUMBLEBEE_*`
    build-config, `BSWAP*` (`std::byteswap` is C++23): as planned.
*Gate:* `BUMBLEBEE_ASSERT` untouched, so the `NDEBUG` compile-out and its death-test gating are
unaffected; 854/854 Release · e2e 99.

### W9 — Concurrency audit — **✅ DONE (audit-and-document, as scoped)**

**✅ The 29 `memory_order_relaxed` uses, audited — all correct, in three safe categories:**
1. **Work-claiming counters** (`next_partition_.fetch_add` in the partitioned aggregate merge,
   `next_page_idx_` morsel claiming, `PhysicalLimit`'s CAS reservation): the counter is the only
   coordination; the data behind each claimed unit is published by the task scheduler's stronger
   synchronization.
2. **Statistics accumulators** (insert/update/delete counts, `SpillCollection::count_`): read
   only after a barrier (Combine under the global mutex / Finalize), never used to publish data.
3. **Best-effort cancellation flags** (`dead_`, `stop_`): a stale read delays a stop by one chunk
   — the documented best-effort-cancellation semantics.
   `executor.cpp:178` already documents its upgrade condition ("must become release if the queue
   is ever made lock-free") — the exact pattern this audit asks for, pre-existing.
- **False sharing / 0 `alignas`:** measure-first per the plan; the A/B machinery this session
  demonstrated (q32's baseline drifting 7 s → 28 s between runs) cannot resolve padding-sized
  effects, so padding stays out — adding unmeasurable `alignas` would be a layout change on
  faith, which the brief forbids.
- **Lock ordering, documented here:** the pool latch (`BufferPoolManager::latch_`) is released
  before any frame `rwlatch_` wait (`EvictOneLocked` hands off via `flushing_` + `flush_cv_`, so
  pool-latch → frame-latch never nests the other way); catalog latch is leaf-level (no calls out
  while held); B+tree uses latch crabbing (parent released once the child is safe), with page
  guards enforcing the discipline by construction.
*Gate:* **TSan on the full 854-test unit suite — clean** (run for the first time as part of W7,
which shares this gate).

### W10 — Dead code and obsolete comments — **✅ DONE**

Triage narrowed 37 raw candidates to 20 genuinely unreferenced, in two passes that each corrected
the detector:

1. `Name(` missed **template calls** — `GetAs<double>(` is used constantly. 37 → 22.
2. It also missed **function-pointer targets**: `allocator.cpp:35` passes `DefaultAllocate`,
   `DefaultFree` and `DefaultReallocate` as bare identifiers with no parens. Searching for the bare
   word instead dropped those. 22 → 20.

**Removed — 20 items, compiler-verified dead:**
- 17 unreferenced accessors/helpers (`CharacterIsNewline`, `Edges`, `ExpressionCount`,
  `GetAggregateAt`, `GetAuxiliary`, `GetDatabase`, `GetNumDeletes`, `GetPrivateData`,
  `GetStartTime`, `GetTableName`, `GetUndoLogNum`, `GetUnlinedColumns`, `IsLittleEndian`,
  `IsRootPage`, `NumOperators`, `QueryProfile`, `Used`).
- `PhysicalOperator::FinalizeStageCount` / `FinalizeMaxThreads` — `virtual` hooks of a staged
  parallel-finalize protocol that is never invoked and never overridden. The contrast is telling:
  `SinkOrderDependent`, declared on the next line, *is* live (overridden in
  `physical_result_collector.h`, called from `pipeline.cpp`). These two were speculative API.
- Four empty `Verify()` stubs (`Vector` ×2, `DataChunk`, `ChunkCollection`) — declared, defined as
  no-ops, never called.

**Obsolete comment removed:** `chunk_collection.h` claimed *"needs VectorOperations —
DataChunk::Cast throws for now"*. `DataChunk::Cast` is fully implemented on top of
`VectorOperations::Cast` and does not throw. The comment told a reader the opposite of the truth.

**TODOs: 17 → 9.** The eight removed were stale "milestone-2" markers on code that has since been
implemented. The nine kept are genuine, live limitations already documented in `CLAUDE.md`
(no WAL, scalar B+tree insert, no statistics-based operator selection, GC not run automatically,
best-effort async cancellation, single-pass external merge, constant LIST/STRUCT normalify).

*Verified:* 772/772 Release · 772/772 ASan · **UBSan clean** · e2e 98 + 101 smallvec + 103 durable.

### W11 — Portability — **☐ BLOCKED LOCALLY (CI workflow written; needs a push to execute)**
The CI workflow added in W1 targets `ubuntu-latest`, so the Linux/GCC leg *exists on paper* — but
it has never run: executing it requires pushing to GitHub, which is outside this local refactor's
authority. Nothing here has been compiled by any toolchain other than AppleClang, and the first
Linux run should be expected to surface real breakage, not to go green. Two session additions
lean on GNU-attribute support, both fine on GCC ≥ 11 and any clang: `[[gnu::always_inline]]`
(physical_type_dispatch.h, vector_copy would-have) and `std::make_unique_for_overwrite` (C++20,
libstdc++ since GCC 11). **This is the one workstream that cannot be completed from this
machine**; the first CI run closes it.

Every build so far: **macOS / arm64 / AppleClang only**. The project clearly supports more —
`CMakeLists.txt` force-includes `std_prelude.hpp` *because libstdc++ does not transitively expose
the same headers as libc++*, and links `uuid` on non-Apple. Until a GCC/libstdc++/Linux/x86-64 build
runs, **no "the build is clean" claim in this document covers any toolchain but AppleClang/arm64.**

### W12 — UndefinedBehaviorSanitizer — **✅ DONE (UBSan clean)**

**✅ Confirmed no build-system change needed** — `CMakeLists.txt` passes `-DSANITIZER=<name>`
straight to `-fsanitize=`, so `-DSANITIZER=undefined` configures and builds as-is. Now in CI.

**✅ First-ever UBSan run: 772/772 tests pass, and it found real, pre-existing undefined
behaviour.** The tests passing is precisely why this went unnoticed — the UB is silent today
because ARM/x86 happen to do the benign thing, but the optimiser is entitled to assume it cannot
occur.

| # | Finding | Location |
|---|---|---|
| U1 | **signed integer overflow** `INT_MIN + INT_MIN` | `arith_operators.h:33` (`Sum`) |
| U2 | **signed integer overflow** `INT_MIN * 5` | `arith_operators.h:49` (`Dot`) |
| U3 | **signed integer overflow** `INT_MIN - 5` | `arith_operators.h:57` (`Difference`) |
| U4 | **misaligned load**, `long long` from a byte buffer | `decimal_column_reader.h:65` |
| U5 | **misaligned load**, `const int32_t` ×2 | surfaced via `mvcc_stress_test.cpp:39`, `serializable_test.cpp:38` |

**✅ All five fixed; UBSan now reports nothing and 772/772 still pass.**

- **U1–U3 — signed overflow.** Fixed by doing the arithmetic in the matching unsigned type and
  converting back, guarded by `if constexpr (IS_SIGNED_INTEGER<T>)` so the discarded branch is
  never instantiated for `float` / `double` / `string_t`. C++20 mandates two's complement and
  modular signed conversion, so this is fully defined and yields **bit-for-bit the wraparound the
  engine already relied on** — no observable change, as the brief requires.

  The alternative — `__builtin_*_overflow` and throw — is arguably the more correct SQL semantics,
  but it *would* change behaviour (queries that wrap silently today would start erroring). Left as
  an open product decision; it would want `.slt` coverage of overflow cases.

  **Proven free:** recompiling `vector_arith.cpp` before and after gives **byte-identical object
  files and zero differences across 2,265,998 disassembled instructions.** The casts vanish.

- **U4–U5 — misaligned loads.** Replaced `reinterpret_cast` + dereference with `std::memcpy` into a
  local, in `decimal_column_reader.h` (both `DictRead` and `PlainRead`) and in the two MVCC test
  helpers. On the targeted architectures this lowers to the same load.

  **A second, worse bug found while fixing this.** In `PlainRead`, `PHYSICAL_TYPE` comes from the
  DECIMAL's *declared* width but `byte_len` comes from the *file's* schema type, and the two are
  chosen independently. A DECIMAL declared BIGINT but stored as parquet INT32 therefore loaded 8
  bytes after `Available(4)` had only validated 4 — a 4-byte read past the checked bound. The copy
  is now clamped to `min(byte_len, sizeof(PHYSICAL_TYPE))`, which is bit-identical whenever the
  widths agree (every case the corpus exercises) and no longer over-reads when they do not.

  *Was still open, resolved by W2:* the "declared wider than stored" zero-fill-vs-sign-extend
  worry turned out to be unreachable within one file — the reader's width is *derived from* the
  storage type (INT32 → DECIMAL(9,s), INT64 → DECIMAL(18,s)), so the two cannot disagree. What W2
  found instead is the cross-schema variant (table-declared width vs file width), which was a
  worse, reachable bug — see the W2 section. The clamp stays as defence in depth.

*Verified:* UBSan clean · 772/772 Release · 772/772 ASan · TSan 68/68 · e2e 98 + 101 smallvec +
103 durable · 11 parquet/decimal e2e · disassembly identical.

---

## 6. Explicitly not doing (with measurements)

- **The last 3 anonymous namespaces** — template *policy functors*. A type has no `static` spelling
  for internal linkage ([basic.link]); nesting in a class does not help (verified:
  `Kernel<Holder::NestedPolicy>` still emits an external `T`). Promoting measured **+58% static lib
  (25.5→40.3 MB), +34% binary, +28,673 symbols**; keeping costs +366 symbols and +0.24%.
- **`node_tag_to_string.cpp`'s 814-line switch** — a flat enum→string table.
- **`LaneMin`/`LaneMax`** — NaN-dropping builtins chosen deliberately for SIMD lanes the strict `<`
  form will not produce.
- **The 617 public data members** — deliberately struct-oriented (bustub lineage).
- **`DataChunk`/`Vector` out-parameters** (S4) — they reuse caller-owned buffers by design.
- **On-disk format, manifest protocol, `--test-protocol` wire format** — frozen.

---

## 7. Verification protocol

1. **Build** Release, ASan, TSan, small-vector (+ UBSan per W12, + GCC/Linux per W11).
2. **Test** unit (Release + ASan), e2e × 3 modes, TSan for concurrency-touching changes.
3. **Symbols** — `nm` diff of `libbumblebee_lib.a`; new exports explained, removals intentional.
4. **Warnings** — changed files with `-Wall -Wextra` before/after; counts **identical file-by-file**.
   The project does not build with these, so absolute counts are meaningless — only parity is.
5. **Codegen, hot paths** — object size, symbol count, `-Rpass=loop-vectorize` remark count.
6. **Differential behaviour** — EXPLAIN plans, CLI invocations, query results.
7. **Benchmarks** — **interleaved A/B/A/B on the same machine**, never a stored baseline. This
   machine drifts: clickbench q33 climbed 5.70 → 7.71 → 9.66 s across runs of *unchanged* code.
   Report within-arm spread beside the between-arm delta; if noise ≥ delta, the honest conclusion is
   "no measurable change".

**Rollback rule:** anything that cannot clear its gate is reverted, not weakened. Tests are never
modified to make a refactor pass — only when the test itself is wrong (as with the `NDEBUG` death
test), with justification in the commit message. Exercised three times: the Copy split (W3),
`generic_key.h` and `aggregate_state.h` (W4) — each revert proven complete by object byte-identity.

**Final verification (the completed tree, all workstreams closed):**
854/854 unit — Release · ASan · UBSan (clean) · **TSan, full suite** · e2e 99 + 102 small-vector +
104 durable · changed-line clang-format clean tree-wide · A/B benchmark run in both interleave
orders (no measurable change). Outstanding: W11's first Linux CI run (needs a push).

---

## 8. Sequencing

```
✅ W1  (tooling + CI)      DONE
✅ W12 (UBSan)             DONE — 5 UB findings fixed, sanitizer clean and in CI
✅ W10 (dead code)         DONE — 20 dead items, TODOs 17 → 9
✅ W2  (operator tests)    DONE — sort/topn, hash join, hash aggregate, nested loop join,
                           grace hash join, external merge sort, insert/update/delete,
                           table scan, parquet scan (incl. the decimal-width guard)
✅ W3  (long fns)          DONE — 8 split (ctor rename, BindCreate, PlanSubqueryPredicate,
                           PlanCorrelatedScalarSubquery, PlanExpression, PruneColumns,
                           OptimizeJoinOrder, ParquetWriter::Flush); the 4 † hot-path fns resolved
                           by experiment: Copy's split measured against the codegen gate and
                           REVERTED (796/1450 instructions changed, +16B frame); Evaluate /
                           NLJ Execute / Normalify kept with recorded verdicts (enumeration /
                           state machine / enumeration)
✅ W4  (763 labels)        DONE — physical_type_dispatch.h; 6 files converted (3 byte-identical,
                           3 within gate + A/B "no measurable change" in both run orders);
                           2 header attempts reverted; non-convertible families recorded

✅ W5  (const/API)        DONE — for_overwrite ×12, [[nodiscard]] on Try* (+8 explicit voids),
                          const_cast/noexcept audits clean; explicit sweep → CI tidy
✅ W7  (shared_ptr)       DONE — buffer-pool trio single-owner, guards non-owning
                          (3 fewer atomic refcount pairs per page access); full-suite TSan clean
✅ W8  (macros)           DONE — DISALLOW_* → = delete ×11, dead MappingType deleted;
                          template-head + page-geometry macros kept with recorded reasons
✅ W9  (concurrency)      DONE — 29 relaxed orders audited into 3 safe categories, lock ordering
                          documented; padding rejected (unmeasurable on this machine)
✅ W6  (headers)          CLOSED — dropped by its own gate (A/A clean builds differ 31 %);
                          the <iostream> eviction candidate recorded for CI/quieter hardware
☐ W11 (GCC/Linux in CI)  BLOCKED LOCALLY — workflow ready; executing it needs a push to GitHub,
                          which is the one action outside this refactor's local authority
```

Why this order held up in practice:
- **W1 first** — free, and it immediately caught ~812 lines of continuation-line misalignment that
  *this refactor* had introduced.
- **W12 early** — and it paid: UBSan found signed overflow in the SQL arithmetic operators and an
  out-of-bounds read in the parquet decimal reader, both pre-existing, both invisible to a green
  test suite. Fixing them after W3/W4 had built on those files would have been worse.
- **W10 before W3** — no point splitting a 150-line function that turns out to be dead.
- **W2 before W3/W4** — otherwise both are refactored under e2e coverage only. Already vindicated:
  writing the join tests surfaced a constructor-parameter naming defect (see W3) that would
  otherwise have been carried straight through a refactor of that file.
