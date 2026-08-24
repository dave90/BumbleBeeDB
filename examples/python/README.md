# Python examples

Each numbered script is standalone and is also executed by pytest, so the examples track the public
API:

- `01_getting_started.py` — in-memory SQL, tuples, and DataFrames;
- `02_persistent_database.py` — durable close/reopen;
- `03_dataframe_analytics.py` — DataFrame loading, joins, nullable data, and aggregates;
- `04_transactions.py` — independent sessions, snapshots, rollback, and conflicts;
- `05_concurrent_queries.py` — a shared `Database` and the global native worker cap;
- `06_external_parquet.py` — external reads/writes, plans, and vacuum;
- `07_null_and_type_handling.py` — nullable and exact Python/pandas types;
- `08_query_plans_and_scripts.py` — explain, scripts/files, and structured errors.

After installing the project with its DataFrame dependencies, run any example directly:

```bash
python examples/python/01_getting_started.py
```
