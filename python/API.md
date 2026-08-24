# BumbleBeeDB Python API contract

This document freezes the first-release Python surface. Native and binding changes must preserve
these semantics unless an intentional contract change is documented here.

## Package and construction

The distribution and import package are both named `bumblebeedb`:

```python
import bumblebeedb as bb

memory_db = bb.db()
durable_db = bb.db("analytics.bbdb")
```

`bb.db` accepts `path=None`, `worker_threads=0`, `max_memory=None`, `frames=None`,
`morsel_pages=None`, `transaction_timeout=None`, and `prefer_external=False`. Unknown options,
booleans supplied for integer options, negative limits, zero frame or morsel counts, and invalid
paths raise `ValueError` (or the appropriate `OSError` subclass for an operating-system failure).
`worker_threads=0` selects an automatic database-wide limit. `morsel_pages` overrides execution
granularity and is intended primarily for deterministic testing and advanced tuning. A new database
is empty; demo tables are never created.

## Ownership and concurrency

- `Database` is safe to share between Python threads. Each `Database.sql` call uses an independent
  temporary native connection and may overlap other calls.
- `Connection` owns at most one explicit transaction and is a sequential session. Concurrent
  statement calls on one connection raise `ProgrammingError`; they are never interleaved.
- `Database.sql` executes exactly one statement in autocommit mode. Transaction-control statements
  on it raise `ProgrammingError` and direct callers to `Database.connect()`.
- `Connection.sql` executes exactly one statement. `execute_script` executes several statements on
  one connection and returns one `Result` per statement, in order.
- `Database.execute_script` and `Database.run_file` create one temporary `Connection`, so explicit
  `BEGIN`/`COMMIT` in the script use one session.
- A script that opens a transaction and reaches end-of-file without `COMMIT` or `ROLLBACK` is rejected
  with `ProgrammingError` and rolled back. A script invoked on a `Connection` that was already in a
  transaction may continue that caller-owned transaction.
- Closing a connection rolls back an open transaction. Closing a database waits for admitted work,
  invalidates its connections, flushes durable state, and is idempotent.
- `Result` owns immutable native data. Full-result conversion is repeatable and remains valid after
  later statements, table changes, connection close, or database close. Cursor-style fetch methods
  have a separate mutable position and are not thread-safe.

## Exceptions

All package exceptions derive from `BumbleBeeError`:

- `DatabaseError` for database/engine failures;
- `DatabaseClosedError` for work attempted after close;
- `ParserError`, `BinderError`, `PlannerError`, and `ExecutionError` for the corresponding SQL stage;
- `ConflictError` (an `ExecutionError`) for transaction/write conflicts;
- `NotImplementedError` for recognized but unsupported engine behavior;
- `ProgrammingError` for API misuse, including concurrent use of one `Connection`;
- `DataError` for unsupported or invalid values and DataFrame conversion failures.

Python argument validation uses Python's built-in `TypeError` and `ValueError` where appropriate.

## Transactions

`Connection.transaction(isolation="snapshot")` supports `snapshot` and `serializable`. It commits
on a normal context exit and rolls back on an exception. Nested transactions, commit/rollback
without an active transaction, and DDL inside an explicit transaction raise `ProgrammingError`.
The equivalent explicit methods are `begin(isolation=...)`, `commit()`, and `rollback()`.

## Table, script, and plan helpers

`get_tables()` returns a name-sorted list of table dictionaries. `describe_table(name)` returns the
same structure for one table:

```python
{
    "name": "sales",
    "columns": [{"name": "sale_id", "type": "BIGINT", "primary_key": True}],
    "primary_key": ["sale_id"],
    "generated_id": False,
    "storage": "row",          # or "parquet"
    "location": None,          # external parquet directory when applicable
    "estimated_rows": 0,
}
```

`get_table(name)` performs an injection-safe `SELECT *`; `remove_table(name, if_exists=False)`
returns whether a table was dropped. `vacuum(name)` applies only to an external parquet table and
returns the number of orphan/superseded files removed. `explain(query, mode="physical")` returns plan
text and accepts `binder`, `planner`, `optimizer`, `physical`, `pipelines`, and `analyze`.

`execute_script(text)` and `run_file(path)` return `list[Result]`. Files are decoded strictly as
UTF-8, scripts stop at their first error, and script errors report a one-based statement index.

`collect_garbage()` may be called on either surface to run transaction timeout and version
reclamation immediately. It returns `{"timed_out": int, "reclaimed": int}`. The diagnostics-only
`Database.resource_stats()` returns database-wide `worker_capacity`, `active_worker_slots`,
`peak_worker_slots`, `query_memory_used`, `query_memory_peak`, and `active_operations` counters.
These controls exist to make lifecycle/resource guarantees observable; ordinary applications do not
need to call them.

## DataFrames

`load_df(frame, name, *, primary_key=None, if_exists="error", include_index=False)` copies all data
before returning. `if_exists` supports only `error` and `append`. The caller must not mutate a frame
while it is being loaded; no pandas or NumPy object is retained afterward. `to_df()` returns a new,
independent pandas `DataFrame` on every call.

## Native/GIL boundary

The binding may hold the GIL only while validating or converting Python-owned objects and while
constructing Python results. Parsing, binding, planning, execution, transactions, waits, I/O,
shutdown, native result preprocessing, and native insertion run with the GIL released. Native engine
code must never include Python headers, retain Python objects, invoke Python callbacks, or acquire the
GIL from a worker thread.
