"""Minimal true-parallel shared-Database tests."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor

import pytest

import bumblebeedb as bb


@pytest.mark.concurrency
def test_one_database_serves_two_python_threads_repeatedly() -> None:
    with bb.db(frames=128, worker_threads=2) as database:
        database.sql("CREATE TABLE numbers(value INTEGER PRIMARY KEY)")
        database.sql("INSERT INTO numbers VALUES " + ",".join(f"({value})" for value in range(100)))

        def query(lower_half: bool) -> int:
            result = 0
            for _ in range(20):
                predicate = "value < 50" if lower_half else "value >= 50"
                result = database.sql(
                    f"SELECT sum(value) FROM numbers WHERE {predicate}"
                ).tuples()[0][0]
            return result

        for _ in range(10):
            with ThreadPoolExecutor(max_workers=2) as pool:
                assert list(pool.map(query, (True, False))) == [1225, 3725]
