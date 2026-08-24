"""Database/Connection shutdown races through the Python API."""

from __future__ import annotations

import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

import bumblebeedb as bb


def _populate(database: bb.Database, rows: int = 500) -> None:
    database.sql("CREATE TABLE numbers(value INT PRIMARY KEY)")
    database.sql("INSERT INTO numbers VALUES " + ",".join(f"({value})" for value in range(rows)))


def _long_query(target: bb.Database | bb.Connection) -> tuple[int]:
    return target.sql(
        "SELECT SUM(a.value + b.value + c.value) FROM numbers a, numbers b, numbers c"
    ).tuples()[0]


def _wait_for_active_operation(database: bb.Database, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if database.resource_stats()["active_operations"] > 0:
            return
        time.sleep(0)
    raise AssertionError("native operation did not become active")


@pytest.mark.concurrency
def test_database_close_waits_for_query_and_rejects_late_calls() -> None:
    database = bb.db(frames=512, worker_threads=2)
    _populate(database)
    result: list[tuple[int]] = []
    query = threading.Thread(target=lambda: result.append(_long_query(database)))
    query.start()
    _wait_for_active_operation(database)

    closer = threading.Thread(target=database.close)
    closer.start()
    deadline = time.monotonic() + 5
    while True:
        try:
            database.sql("SELECT 1")
        except bb.DatabaseClosedError:
            break
        if time.monotonic() >= deadline:
            raise AssertionError("database never entered closing state")

    query.join(timeout=10)
    closer.join(timeout=10)
    assert not query.is_alive() and not closer.is_alive()
    assert result
    assert database.closed


@pytest.mark.concurrency
def test_two_close_callers_and_connect_racing_close_do_not_deadlock() -> None:
    database = bb.db(frames=128)
    start = threading.Barrier(3)

    def close() -> None:
        start.wait(timeout=5)
        database.close()

    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(close) for _ in range(2)]
        start.wait(timeout=5)
        for future in futures:
            future.result(timeout=5)
    assert database.closed
    with pytest.raises(bb.DatabaseClosedError):
        database.connect()


@pytest.mark.concurrency
def test_connection_close_racing_its_statement_waits_safely() -> None:
    with bb.db(frames=512, worker_threads=2) as database:
        _populate(database)
        connection = database.connect()
        result: list[tuple[int]] = []
        query = threading.Thread(target=lambda: result.append(_long_query(connection)))
        query.start()
        _wait_for_active_operation(database)
        closer = threading.Thread(target=connection.close)
        closer.start()

        query.join(timeout=10)
        closer.join(timeout=10)
        assert not query.is_alive() and not closer.is_alive()
        assert result and connection.closed
        with pytest.raises(bb.ProgrammingError, match="closed"):
            connection.sql("SELECT 1")
