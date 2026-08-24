<div align="center">

<h1>🐝 BumbleBeeDB</h1>

<p><strong>A compact embedded SQL database, built from scratch in C++20.</strong></p>

<p>Vectorized execution · MVCC transactions · persistent storage · writable Parquet · Python API</p>

<p>
  <a href="https://github.com/dave90/BumbleBeeDB/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/dave90/BumbleBeeDB/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="C++ 20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="Python 3.10–3.13" src="https://img.shields.io/badge/Python-3.10--3.13-3776AB?logo=python&logoColor=white">
  <a href="./LICENSE"><img alt="GPLv3 license" src="https://img.shields.io/badge/License-GPLv3-f5c518"></a>
</p>

</div>

BumbleBeeDB is an embedded, in-process relational database for applications, experiments, and
learning how a database works end to end. Open an in-memory database or a persistent file, run SQL
from Python or the interactive shell, and query ordinary tables alongside Parquet datasets.

> [!NOTE]
> BumbleBeeDB is an early-stage project under active development. It is not yet intended for
> production workloads.

## Why BumbleBeeDB?

- **Embedded and zero-server** — use an ephemeral database or persist everything to one local path.
- **Vectorized and parallel** — a chunk-at-a-time, push-based execution engine runs work across a
  bounded worker pool.
- **Concurrent transactions** — MVCC provides independent snapshots, snapshot/serializable
  isolation, rollback, and write-conflict detection.
- **Parquet-native** — attach a directory as a table, infer its schema, prune row groups, skip unused
  columns, and use `INSERT`, `UPDATE`, or `DELETE` with copy-on-write files.
- **Python-friendly** — owning query results, pandas import/export, typed exceptions, and a
  thread-shareable `Database` object.
- **Built to explore** — a custom binder, planner, optimizer, executor, catalog, buffer manager, and
  storage engine sit behind a SQL parser based on DuckDB's `libpg_query` fork.

## Quick start with Python

Python 3.10–3.13, CMake 3.31+, and a C++20 compiler are required when installing from source. Linux
also needs the `libuuid` development headers (`uuid-dev` on Ubuntu/Debian).

```bash
git clone https://github.com/dave90/BumbleBeeDB.git
cd BumbleBeeDB
python -m pip install ".[dataframe]"
```

```python
import bumblebeedb as bb

with bb.db() as db:  # pass "analytics.bbdb" here to persist the database
    db.sql("CREATE TABLE readings(sensor_id INT PRIMARY KEY, value DOUBLE)")
    db.sql("INSERT INTO readings VALUES (1, 18.5), (2, NULL)")

    result = db.sql("SELECT sensor_id, value FROM readings ORDER BY sensor_id")
    print(result.tuples())       # [(1, 18.5), (2, None)]
    frame = result.to_df()       # independent pandas DataFrame
```

Row tables always have a primary key. If you do not declare one, BumbleBeeDB adds and fills a
generated `BIGINT _id` column.

Use a connection when multiple statements belong to one transaction:

```python
with bb.db() as database:
    database.sql("CREATE TABLE accounts(id INT PRIMARY KEY, balance INT)")
    database.sql("INSERT INTO accounts VALUES (1, 100), (2, 100)")

    with database.connect() as connection:
        with connection.transaction(isolation="snapshot"):
            connection.sql("UPDATE accounts SET balance = balance - 10 WHERE id = 1")
            connection.sql("UPDATE accounts SET balance = balance + 10 WHERE id = 2")
```

See the [Python examples](./examples/python) for persistence, DataFrames, concurrent queries,
transactions, Parquet, query plans, and scripts.

## Quick start with the SQL shell

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run one query in memory
./build/BumbleBee -m -c "SELECT 1 + 1 AS answer;"

# Or open the interactive, file-backed shell (uses ./bb.db by default)
./build/BumbleBee
```

Statements may span several lines and run when a line ends in `;`. Type `\help` for shell commands,
use `--db <path>` to choose a database file, or `--memory` / `-m` to keep everything in memory.

## Query Parquet like a table

```sql
-- Declare a schema, or use an empty column list to infer it from existing files.
CREATE TABLE events (id BIGINT, payload VARCHAR)
WITH (format = 'parquet', location = '/data/events');

INSERT INTO events VALUES (1, 'created'), (2, 'processed');
SELECT payload FROM events WHERE id > 1;
```

External Parquet writes are copy-on-write and intentionally unavailable inside explicit
transactions. `DROP TABLE` only removes the catalog entry—your Parquet files remain yours.

## Project map

| Looking for… | Start here |
|---|---|
| Complete Python programs | [`examples/python/`](./examples/python) |
| Python API behavior | [`python/API.md`](./python/API.md) |
| Development conventions | [`DEVELOP.MD`](./DEVELOP.MD) |
| SQL end-to-end test format | [`test/e2e/README.md`](./test/e2e/README.md) |

## License

BumbleBeeDB is available under the [GNU General Public License v3.0](./LICENSE).
