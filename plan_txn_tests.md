# Plan: expanded e2e transaction tests

> **Status: IMPLEMENTED** (all three phases + docs). Full matrix green: 672 unit tests, e2e on the
> normal and small-vector builds, concurrency suite under TSan. The engine bugs found along the way
> were subsequently FIXED: self-referencing INSERT...SELECT (Halloween problem — insert sink now
> defers writes to Combine like Update/Delete), untyped NULL literals (UNKNOWN type, casts as a NULL
> broadcast) + IS [NOT] NULL, and int64-range integer literals. Pinned in
> dml/insert_select_self.slt and scan_filter/null_predicates.slt.

Current coverage: `test/e2e/slt/transactions/` has two files — `commit_rollback.slt` (basic
BEGIN/COMMIT/ROLLBACK, own-write visibility for INSERT/UPDATE/DELETE, START TRANSACTION) and
`transaction_errors.slt` (stray COMMIT/ROLLBACK, nested BEGIN, statement failure aborting the txn,
DDL-not-transactional). Everything is single-session, tiny INT tables, and no timeout coverage.

Unit-level concurrency is already strong (`test/unit/concurrency/`: mvcc_stress, serializable,
transaction, watermark — TSan-gated), so the e2e plan focuses on *deterministic, SQL-visible*
behavior, not races.

## Phase 1 — sequential tests, no infrastructure needed (write today)

### 1a. `txn_types.slt` — transactions across data types
The MVCC undo path re-homes string payloads; only INT has ever been exercised through e2e txns.
- Table with INT, BIGINT, VARCHAR, and NULLable columns.
- ROLLBACK of INSERTs with: long VARCHARs (> 11 bytes, forcing StringHeap payloads, not inlined),
  empty strings, NULLs in every column, BIGINT boundary values (min/max).
- UPDATE inside a txn that changes value→NULL and NULL→value, then ROLLBACK — verify originals
  restored exactly (including NULL rendering).
- UPDATE that grows a string (inlined → heap) and shrinks one (heap → inlined), COMMIT and
  ROLLBACK variants — this stresses version-chain string ownership.
- DELETE + ROLLBACK restores rows with string/NULL payloads intact.

### 1b. `txn_bulk.slt` — more data, multi-chunk paths
- Insert well over `STANDARD_VECTOR_SIZE` rows inside one txn (e.g. a few thousand via repeated
  multi-row INSERTs), verify `COUNT(*)`/`SUM` inside the txn, then ROLLBACK and verify 0.
- Same shape with COMMIT; then a second txn UPDATEs a large slice (`WHERE v % 2 = 0`) and rolls
  back — aggregate checks confirm full restoration.
- A companion `# require: small_vector` file (or reuse the same file gated) so the corpus also
  runs multi-chunk with a handful of rows on `build-smallvec`.
- Large DELETE (most of the table) + ROLLBACK, then + COMMIT.

### 1c. `txn_sequential.slt` — chains of transactions in one session
- Several BEGIN/COMMIT cycles building on each other, interleaved with autocommit statements —
  verify each txn's snapshot sees all previously committed work.
- ROLLBACK then redo the same work and COMMIT (exercises version chains on the same rows/PKs).
- Delete + re-insert the same PK within one txn; commit; verify final state.
- Multi-table txn: writes to 2–3 tables, one ROLLBACK undoes all of them atomically.
- Reads through real operators inside a txn: JOIN, GROUP BY, ORDER BY/TopN, DISTINCT over a mix
  of committed and own-uncommitted rows (own-write visibility through the whole pipeline, not
  just a bare scan).
- Aborted txn (duplicate-PK error) followed immediately by a fresh successful txn on the same
  table — the abort must not poison later transactions.

## Phase 2 — concurrent transactions (needs multi-session infrastructure)

**Blocker:** `BumbleBeeInstance` holds a single `active_txn_` and the shell is one session per
process; two processes can't share one embedded DB. So e2e concurrency needs *named sessions
inside one shell process*.

**Key insight that keeps this small:** "concurrent transactions" does *not* require parallel
threads. Statements still execute one at a time in the shell's single loop; what makes them
concurrent is that they run inside *different open transactions*. MVCC then supplies all the
interesting semantics (snapshots, first-committer-wins) deterministically. True thread-level
races stay in the TSan unit tests.

The only session-scoped state in the instance today is that one pointer — `active_txn_`
(`bumblebee_instance.h:232`), touched in `HandleTransactionStatement`
(`bumblebee_instance.cpp:286`) and the two statement-execution paths (`:347`, `:406`).
Catalog, storage, and the txn manager are already shared and need no change. So:

1. **Instance:** replace the single member with
   `std::unordered_map<std::string, Transaction *> session_txns_` plus
   `std::string current_session_{"default"}`. Every existing `active_txn_` read/write becomes
   a read/write of `session_txns_[current_session_]` (a tiny private accessor keeps the diff
   mechanical). The destructor's cleanup (`bumblebee_instance.cpp:116` aborts the open txn)
   loops over all sessions. Nothing else in the engine changes.
   *(A fuller `Session` object owning per-session config can come later if an embedded client
   API needs it; for e2e tests the map is enough and the refactor stays ~30 lines.)*
2. **Shell:** a `\session <name>` meta-command (same dispatch block as `\dt`/`\help` in
   `ExecuteSql`) that sets `current_session_`, creating the entry on first use. Switching
   sessions never touches the transactions themselves — an open txn simply stays open until
   its session is switched back to.
3. **Harness:** adopt sqllogictest connection labels — `statement ok s1` /
   `query rowsort s2`. `slt_runner.py` parses the label (default `"default"`), remembers the
   last label sent, and emits a `\session <label>` line before a record only when the label
   changes. Interleavings are then fully deterministic and readable directly in the `.slt`
   file:

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

Then these files (deterministic interleavings — true races stay in the TSan unit tests):

### 2a. `txn_concurrent_visibility.slt` — snapshot isolation semantics
- s1 BEGIN + INSERT; s2 (autocommit and in-txn) must not see the uncommitted rows; after s1
  COMMIT, a *new* s2 statement sees them, but a txn s2 opened *before* the commit still doesn't
  (snapshot taken at BEGIN).
- Same for UPDATE (s2 keeps seeing the old version) and DELETE (row still visible to s2's older
  snapshot).
- Two read-only txns over the same table interleaved — both stable.

### 2b. `txn_concurrent_conflicts.slt` — first-committer-wins
- s1 and s2 both BEGIN; s1 UPDATEs row X and COMMITs; s2's UPDATE of row X must fail with the
  write-write conflict `ExecutionException` (`statement error`), and s2's txn is aborted — its
  other writes are gone.
- Same-row DELETE vs UPDATE conflict, and DELETE vs DELETE.
- Disjoint-row updates from two txns both commit fine (no false conflicts).
- Concurrent INSERTs with the same explicit PK: first commit wins, second fails on the
  uniqueness index.
- (Optional, only if `BEGIN SERIALIZABLE` / a `SET` is added to the SQL surface — not currently
  bindable: commit-time validation failure for a scanned-then-written serializable txn.)

## Phase 3 — transaction timeout (needs infrastructure)

**Current state:** `TransactionManager` already has `txn_timeout_` (default 2h) and
`Transaction::IsExpired`, but enforcement only happens inside `GarbageCollection()`, and nothing
in the shell ever drives GC. Needed:

1. `--txn-timeout <ms>` CLI flag threaded to the `TransactionManager` ctor, exposed to `.slt`
   via `# config: txn_timeout=...` (the README already earmarks a txn knob for "when explicit
   transactions land" — they have).

   Note on reducing the timeout for tests: unlike `STANDARD_VECTOR_SIZE`, the timeout is *not* a
   compile-time constant — `TransactionManager` already takes `txn_timeout` as a runtime ctor
   parameter (default `std::chrono::hours(2)`). So no dedicated build variant (à la
   `build-smallvec`) is needed: the CLI flag alone lets each `.slt` file pick its own timeout,
   down to tens of milliseconds, keeping the test wall-clock well under a second instead of a
   minute.
2. A deterministic GC trigger: a `\gc` meta-command (preferred over a background thread, whose
   timing would make tests flaky). Concrete implementation:

   - `GarbageCollection()` already exists and does the work (`transaction_manager.cpp:216`
     aborts expired txns, prunes versions below the watermark); today it simply has **no
     caller** outside unit tests (see the TODO at `transaction_manager.cpp:224`).
   - Add a `\gc` branch in the meta-command block of `BumbleBeeInstance::ExecuteSql`
     (`bumblebee_instance.cpp:456`, alongside `\dt` / `\help` / `\clear`): call
     `txn_mgr_->GarbageCollection();` and write a one-line confirmation (e.g. `GC`) through the
     `ResultWriter`, mirroring how BEGIN/COMMIT echo their keyword. List it in `\help`.
   - **Dangling-session hazard (must handle):** if GC aborts the timed-out transaction that this
     session itself holds open, `active_txn_` still points at a now-ABORTED transaction. Two
     pieces: (a) after running GC, `\gc` checks `active_txn_` and clears it if its state is
     ABORTED; (b) defensively, the statement-execution path (`bumblebee_instance.cpp:347`/`:406`,
     where it picks `autocommit ? Begin() : active_txn_`) throws a clear
     "current transaction was aborted (timeout)" error and clears `active_txn_` when the joined
     txn is no longer RUNNING — this is what the `.slt` `statement error` records assert on.
     (The raw pointer itself stays valid — the manager's map holds txns by `shared_ptr` — so
     this is a state check, not a lifetime problem.)
   - Unit test: `TransactionManager` with a millisecond timeout + fake-advanced clock or a short
     real sleep, assert `\gc`-equivalent (`GarbageCollection()`) aborts only the expired txn.
3. Make the harness's `sleep` directive real (it is currently parsed as a no-op in
   `slt_runner.py`) so a file can outlive a millisecond-scale timeout deterministically.

### 3a. `txn_timeout.slt`
- `# config: txn_timeout=100` (ms). BEGIN, INSERT, `sleep 300`, trigger GC → the next statement
  in the txn (or COMMIT) fails: txn was aborted; its writes are gone; session is back in
  autocommit.
- A txn that commits *within* the timeout is unaffected (control case).
- An expired txn's UPDATE/DELETE are rolled back — prior committed data intact.
- After a timeout-abort, a new txn in the same session works normally.
- Multi-session variant (after Phase 2): s1 expires while s2 stays active — only s1 is aborted;
  GC respects the watermark and s2's snapshot is unharmed.

## Documentation updates (part of each phase's definition of done)

- **`README.md`** — the CLI-flags table (around line 66) gains `--txn-timeout <ms>`; the shell
  section documents the new meta-commands `\session <name>` and `\gc` (what they do and that
  the txn timeout is only enforced when GC runs).
- **`test/e2e/README.md`** — document the three harness extensions where the format is
  specified: connection labels on records (`statement ok s1`, `query rowsort s2`, default
  session `default`), the new `# config: txn_timeout=<ms>` key in the config table (and drop
  the "deliberately not exposed: txn_limit" note — explicit transactions have landed), and
  `sleep` becoming a real delay instead of a parsed no-op.
- **Shell `\help`** (`CmdDisplayHelp` in `bumblebee_instance.cpp`) — list `\session` and `\gc`.
- **`CLAUDE.md`** — the Shell section's flag list gains `--txn-timeout`; the SQL-surface
  limitations bullet on concurrency notes that the txn timeout is GC-driven and testable via
  `\gc`.
- **`TODO.md`** — tick/remove the "run GC automatically" item's *manual trigger* part if listed
  (the TODO at `transaction_manager.cpp:224` about automatic scheduling stays open — `\gc` is a
  manual driver, not the periodic one).

## Execution order & gates

Phase 1 is pure `.slt` authoring — run on both normal and small-vector builds. Phase 2's session
refactor touches `BumbleBeeInstance` state handling → re-run the TSan unit suite. Phase 3 touches
`TransactionManager` construction → same gate. Full matrix (unit + e2e × both builds) before
declaring each phase done, per CLAUDE.md.