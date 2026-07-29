#!/usr/bin/env python3
# ===----------------------------------------------------------------------===
#
#                         BumbleBeeDB
#
# prepare_data.py
#
# Identification: benchmarks/prepare_data.py
#
# ===----------------------------------------------------------------------===
"""Prepare fixtures + generated setup scripts for the benchmark suite.

Everything this writes lives under ``benchmarks/data/`` (gitignored):

* ``data/tpch/<table>/<table>.parquet`` and ``data/clickbench/hits/hits.parquet``
  -- one directory per logical table, each a symlink to a shared parquet fixture.
  BumbleBeeDB adopts a directory of foreign parquet files as an external table
  (``CREATE TABLE t () WITH (format='parquet', location=<dir>)``); DuckDB reads the
  file through a view. Both bind the same bytes, so the comparison is apples-to-apples.
* ``data/setup/*.sql`` -- generated setup scripts with absolute paths baked in.
* ``data/dml/*.sql``   -- generated DML workload statements (bulk load, updates,
  deletes, batched point ops, transaction micro-benchmarks).

Fixtures are taken from the sibling BumbleBee (datalog) benchmark downloads by
default; override with ``BBDB_BENCH_FIXTURES=/path/to/parquet_root`` (expects
``hits.parquet`` and ``tpch/<table>.parquet`` under it).
"""
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
FIXTURES = Path(os.environ.get(
    "BBDB_BENCH_FIXTURES",
    Path.home() / "git" / "BumbleBee" / "benchmarks" / "downloads"))

TPCH_TABLES = ["customer", "lineitem", "nation", "orders", "part", "partsupp", "region", "supplier"]

# Number of rows for the DML bulk-load baseline. Kept modest so a full sweep is quick;
# raise for a heavier DML comparison.
DML_ROWS = int(os.environ.get("BBDB_BENCH_DML_ROWS", "20000"))
# Distinct join buckets in the `dim` table: the build side of the DML-from-a-join tests. Small
# enough that the hash table is cache-resident, so those tests measure the write path plus a
# realistic probe rather than a hash-table-sizing experiment.
DIM_ROWS = int(os.environ.get("BBDB_BENCH_DIM_ROWS", "1000"))


def symlink_table(table: str, src: Path, dst_dir: Path) -> bool:
    """Create ``dst_dir/<table>.parquet`` as a symlink to ``src`` (idempotent)."""
    if not src.exists():
        print(f"  [MISSING] {src} -- skipping {table}")
        return False
    dst_dir.mkdir(parents=True, exist_ok=True)
    link = dst_dir / f"{table}.parquet"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(src.resolve())
    return True


def prepare_parquet() -> tuple[list[str], bool]:
    """Build the per-table parquet directories. Returns (tpch tables present, hits present)."""
    print(f"Fixtures root: {FIXTURES}")
    present = []
    for table in TPCH_TABLES:
        if symlink_table(table, FIXTURES / "tpch" / f"{table}.parquet", DATA / "tpch" / table):
            present.append(table)
    hits = symlink_table("hits", FIXTURES / "hits.parquet", DATA / "clickbench" / "hits")
    return present, hits


def write_olap_setup(tpch_tables: list[str], hits: bool) -> None:
    """Emit the BumbleBeeDB (external table) and DuckDB (view) OLAP setup scripts."""
    setup = DATA / "setup"
    setup.mkdir(parents=True, exist_ok=True)

    def loc(*parts) -> str:
        return str((DATA.joinpath(*parts)).resolve())

    bb, duck = [], []
    if hits:
        bb.append(f"CREATE TABLE hits () WITH (format='parquet', location='{loc('clickbench', 'hits')}');")
        duck.append(f"CREATE VIEW hits AS SELECT * FROM '{loc('clickbench', 'hits', 'hits.parquet')}';")
    for table in tpch_tables:
        bb.append(f"CREATE TABLE {table} () WITH (format='parquet', location='{loc('tpch', table)}');")
        duck.append(f"CREATE VIEW {table} AS SELECT * FROM '{loc('tpch', table, f'{table}.parquet')}';")

    (setup / "bumblebeedb_olap.sql").write_text("\n".join(bb) + "\n")
    (setup / "duckdb_olap.sql").write_text("\n".join(duck) + "\n")
    print(f"OLAP setup: {len(bb)} tables (hits={hits}, tpch={len(tpch_tables)})")


def write_dml() -> None:
    """Generate the DML workload + per-engine schema/seed. k is a dense key; v is a
    deterministic pseudo-random value in [0, 100000) so predicate selectivities are stable."""
    dml = DATA / "dml"
    dml.mkdir(parents=True, exist_ok=True)
    setup = DATA / "setup"

    def val(i: int) -> int:
        return (i * 2654435761) % 100000  # Knuth multiplicative hash, deterministic

    # Bulk load: one multi-row INSERT (single timed statement), shared by both engines.
    # No column list: BumbleBeeDB requires all-columns inserts and auto-fills its _id PK;
    # SQLite's implicit rowid is likewise auto. Both then see (k, v, s) positionally.
    tuples = ",".join(f"({i},{val(i)},'row{i}')" for i in range(DML_ROWS))
    (dml / "load.sql").write_text(f"INSERT INTO bench VALUES {tuples};\n")

    # Predicate update / delete (supported by both engines). v<50000 ~ half the rows.
    (dml / "update_pred.sql").write_text("UPDATE bench SET v = v + 1 WHERE v < 50000;\n")
    (dml / "delete_pred.sql").write_text("DELETE FROM bench WHERE v > 90000;\n")

    # Range point-ish op (supported): a contiguous key window.
    (dml / "point_range.sql").write_text("UPDATE bench SET v = 0 WHERE k >= 1000 AND k < 2000;\n")

    # Batched point op via IN -- NOT yet supported by BumbleBeeDB (xfail); works on SQLite.
    keys = ",".join(str(k) for k in range(0, 2000, 2))
    (dml / "point_in.sql").write_text(f"UPDATE bench SET v = v + 1 WHERE k IN ({keys});\n")

    # Transaction overhead: N autocommit inserts vs the same N inside one transaction.
    n = 1000
    base = DML_ROWS
    auto = "\n".join(f"INSERT INTO bench VALUES ({base + i},{val(base + i)},'txn{i}');"
                     for i in range(n))
    (dml / "txn_autocommit.sql").write_text(auto + "\n")
    single = ("BEGIN;\n" +
              "\n".join(f"INSERT INTO bench VALUES ({base + n + i},{val(base + n + i)},'txn{i}');"
                        for i in range(n)) +
              "\nCOMMIT;\n")
    (dml / "txn_single.sql").write_text(single)

    # --- DML sourced from a JOIN ------------------------------------------------------------
    # Everything above writes literals: the read path is never exercised, so the numbers only
    # measure the write side. These drive the same three operations from a two-table join, so one
    # statement pays for a scan + hash join AND the write. That is the shape a real ETL statement
    # has, and the one where a chunk-at-a-time engine can actually amortize its machinery.
    #
    #   dim  : DIM_ROWS rows, the small (build) side, one row per bucket
    #   fact : DML_ROWS rows, the large (probe) side, each pointing at a bucket
    fact_rows = ",".join(f"({i},{i % DIM_ROWS},'f{i}')" for i in range(DML_ROWS))
    dim_rows = ",".join(f"({b},{b * 7 % 1000},'d{b}')" for b in range(DIM_ROWS))

    # INSERT ... SELECT over the join: reads DML_ROWS facts, joins to dim, writes the matches.
    # `dim.w < 500` keeps roughly half the buckets, so the write is a substantial but not total copy.
    (dml / "insert_join.sql").write_text(
        "INSERT INTO bench SELECT fact.fk, dim.w, fact.fs FROM fact, dim "
        "WHERE fact.bucket = dim.b AND dim.w < 500;\n")

    # UPDATE / DELETE driven by an aggregate over the join. The subquery is uncorrelated, so it is
    # one read of the join feeding the write's predicate.
    (dml / "update_join.sql").write_text(
        "UPDATE bench SET v = v + 1 WHERE v > "
        "(SELECT avg(dim.w) FROM fact, dim WHERE fact.bucket = dim.b);\n")
    (dml / "delete_join.sql").write_text(
        "DELETE FROM bench WHERE v > "
        "(SELECT max(dim.w) FROM fact, dim WHERE fact.bucket = dim.b);\n")

    # Row-matching forms (`WHERE k IN (SELECT ... FROM a, b)`) are the more interesting shape but
    # BumbleBeeDB rejects an IN/EXISTS subquery outside a SELECT's WHERE, so they are carried as
    # xfail in the config until that lands.
    (dml / "update_join_in.sql").write_text(
        "UPDATE bench SET v = v + 1 WHERE k IN "
        "(SELECT fact.fk FROM fact, dim WHERE fact.bucket = dim.b AND dim.w < 100);\n")
    (dml / "delete_join_in.sql").write_text(
        "DELETE FROM bench WHERE k IN "
        "(SELECT fact.fk FROM fact, dim WHERE fact.bucket = dim.b AND dim.w < 100);\n")

    # Per-engine schema. BumbleBeeDB auto-adds a BIGINT _id primary key (B+tree index
    # maintained on insert); SQLite uses its implicit rowid -- both row-format PK stores.
    # The join sources are seeded in setup so they are not part of any timed statement.
    (setup / "bumblebeedb_dml.sql").write_text(
        "CREATE TABLE bench (k BIGINT, v BIGINT, s VARCHAR);\n"
        "CREATE TABLE dim (b BIGINT, w BIGINT, ds VARCHAR);\n"
        "CREATE TABLE fact (fk BIGINT, bucket BIGINT, fs VARCHAR);\n"
        f"INSERT INTO dim VALUES {dim_rows};\n"
        f"INSERT INTO fact VALUES {fact_rows};\n")
    (setup / "sqlite_dml.sql").write_text(
        "CREATE TABLE bench (k INTEGER, v INTEGER, s TEXT);\n"
        "CREATE TABLE dim (b INTEGER, w INTEGER, ds TEXT);\n"
        "CREATE TABLE fact (fk INTEGER, bucket INTEGER, fs TEXT);\n"
        f"INSERT INTO dim VALUES {dim_rows};\n"
        f"INSERT INTO fact VALUES {fact_rows};\n")
    print(f"DML: load={DML_ROWS} rows, txn micro-bench={n} rows, "
          f"join sources fact={DML_ROWS} x dim={DIM_ROWS}")


def main() -> None:
    tpch_tables, hits = prepare_parquet()
    write_olap_setup(tpch_tables, hits)
    write_dml()
    print("\nDone. Run a benchmark, e.g.:")
    print("  python3 benchmark_runner.py configs/duckdb_olap.json      # reference first")
    print("  python3 benchmark_runner.py configs/bumblebeedb_olap.json # then the engine under test")


if __name__ == "__main__":
    main()
