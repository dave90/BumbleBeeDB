"""Regression tests for the native GIL boundary."""

from __future__ import annotations

import threading
from pathlib import Path
from typing import Callable

import numpy as np
import pandas as pd
import pytest

import bumblebeedb as bb


def _assert_python_counter_progress(operation: Callable[[], None], minimum: int = 5_000) -> None:
    starting = threading.Event()
    finished = threading.Event()
    errors: list[BaseException] = []

    def run() -> None:
        starting.set()
        try:
            operation()
        except BaseException as error:  # surface worker failures in the test thread
            errors.append(error)
        finally:
            finished.set()

    worker = threading.Thread(target=run)
    worker.start()
    assert starting.wait(timeout=2)
    progress = 0
    while not finished.is_set():
        progress += 1
    worker.join(timeout=10)
    assert not worker.is_alive()
    if errors:
        raise errors[0]
    assert progress > minimum


@pytest.mark.gil
def test_cpu_heavy_query_does_not_hold_the_gil() -> None:
    with bb.db(frames=128, worker_threads=2) as database:
        database.sql("CREATE TABLE numbers(value INTEGER PRIMARY KEY)")
        database.sql("INSERT INTO numbers VALUES " + ",".join(f"({value})" for value in range(500)))

        starting = threading.Event()
        finished = threading.Event()
        outcome: list[object] = []

        def native_query() -> None:
            starting.set()
            outcome.extend(
                database.sql(
                    "SELECT sum(a.value + b.value + c.value) "
                    "FROM numbers a, numbers b, numbers c"
                ).tuples()
            )
            finished.set()

        worker = threading.Thread(target=native_query)
        worker.start()
        assert starting.wait(timeout=2)

        python_progress = 0
        while not finished.is_set():
            python_progress += 1

        worker.join(timeout=2)
        assert not worker.is_alive()
        assert outcome == [(93_562_500_000,)]
        assert python_progress > 10_000


@pytest.mark.gil
def test_native_dataframe_ingestion_does_not_hold_the_gil() -> None:
    rows = 200_000
    frame = pd.DataFrame(
        {
            "id": np.arange(rows, dtype=np.int64),
            "measure": np.arange(rows, dtype=np.float64),
        }
    )
    with bb.db(frames=2048, worker_threads=2) as database:
        starting = threading.Event()
        finished = threading.Event()

        def native_load() -> None:
            starting.set()
            database.load_df(frame, "gil_load", primary_key="id")
            finished.set()

        worker = threading.Thread(target=native_load)
        worker.start()
        assert starting.wait(timeout=2)

        python_progress = 0
        while not finished.is_set():
            python_progress += 1

        worker.join(timeout=3)
        assert not worker.is_alive()
        assert python_progress > 10_000
        assert database.sql("SELECT count(*) FROM gil_load").tuples() == [(rows,)]


@pytest.mark.gil
@pytest.mark.durable
def test_durable_scan_and_disk_io_do_not_hold_the_gil(database_path: Path) -> None:
    rows = 100_000
    with bb.db(database_path, frames=32, worker_threads=2) as database:
        database.load_df(pd.DataFrame({"value": np.arange(rows, dtype=np.int64)}), "durable_scan")

    with bb.db(database_path, frames=32, worker_threads=2) as reopened:
        outcome: list[tuple[object, ...]] = []
        _assert_python_counter_progress(
            lambda: outcome.extend(reopened.sql("SELECT SUM(value) FROM durable_scan").tuples())
        )
        assert outcome == [(rows * (rows - 1) // 2,)]


@pytest.mark.gil
def test_external_parquet_io_does_not_hold_the_gil(external_table_dir: Path) -> None:
    rows = 75_000
    location = str(external_table_dir).replace("'", "''")
    with bb.db(frames=256, worker_threads=2) as database:
        database.load_df(pd.DataFrame({"value": np.arange(rows, dtype=np.int64)}), "source")
        database.sql(
            f"CREATE TABLE external_values(value BIGINT) "
            f"WITH (format='parquet', location='{location}')"
        )
        database.sql("INSERT INTO external_values SELECT value FROM source")
        outcome: list[tuple[object, ...]] = []
        _assert_python_counter_progress(
            lambda: outcome.extend(database.sql("SELECT SUM(value) FROM external_values").tuples())
        )
        assert outcome == [(rows * (rows - 1) // 2,)]


@pytest.mark.gil
def test_close_wait_and_result_preprocessing_do_not_hold_the_gil() -> None:
    with bb.db(frames=512, worker_threads=2) as database:
        database.sql("CREATE TABLE numbers(value INT PRIMARY KEY)")
        database.sql("INSERT INTO numbers VALUES " + ",".join(f"({value})" for value in range(500)))

        query = threading.Thread(
            target=lambda: database.sql(
                "SELECT SUM(a.value + b.value + c.value) FROM numbers a, numbers b, numbers c"
            )
        )
        query.start()
        while database.resource_stats()["active_operations"] == 0:
            pass
        _assert_python_counter_progress(database.close)
        query.join(timeout=10)
        assert not query.is_alive() and database.closed

    rows = 100_000
    with bb.db(frames=512, worker_threads=2) as results_database:
        results_database.load_df(pd.DataFrame({"value": np.arange(rows, dtype=np.int64)}), "many_rows")
        outcome: list[bb.Result] = []
        _assert_python_counter_progress(
            lambda: outcome.append(results_database.sql("SELECT _id, value FROM many_rows"))
        )
        assert len(outcome[0]) == rows
