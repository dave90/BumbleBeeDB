"""Database-wide worker, memory, timeout, and cleanup counter tests."""

from __future__ import annotations

import threading
from concurrent.futures import ThreadPoolExecutor

import pytest

import bumblebeedb as bb


@pytest.mark.concurrency
def test_worker_capacity_is_global_and_slots_return_to_zero() -> None:
    with bb.db(frames=512, worker_threads=2) as database:
        database.sql("CREATE TABLE numbers(value INT PRIMARY KEY)")
        database.sql("INSERT INTO numbers VALUES " + ",".join(f"({value})" for value in range(250)))
        start = threading.Barrier(6)

        def aggregate(_: int) -> int:
            start.wait(timeout=5)
            return database.sql("SELECT SUM(a.value + b.value) FROM numbers a, numbers b").tuples()[0][0]

        with ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(aggregate, range(6)))
        assert len(set(results)) == 1
        stats = database.resource_stats()
        assert stats["worker_capacity"] == 2
        assert 1 <= stats["peak_worker_slots"] <= 2
        assert stats["active_worker_slots"] == 0
        assert stats["query_memory_used"] == 0


def test_low_memory_query_spills_or_finishes_within_global_budget() -> None:
    with bb.db(frames=256, worker_threads=2, max_memory=4096, prefer_external=True) as database:
        database.sql("CREATE TABLE sortable(value INT PRIMARY KEY)")
        database.sql("INSERT INTO sortable VALUES " + ",".join(f"({value})" for value in range(1_000)))
        result = database.sql("SELECT value FROM sortable ORDER BY value DESC LIMIT 5")
        assert result.tuples() == [(999,), (998,), (997,), (996,), (995,)]
        stats = database.resource_stats()
        assert stats["query_memory_peak"] <= 4096
        assert stats["query_memory_used"] == 0


def test_exception_releases_worker_and_query_memory_resources() -> None:
    with bb.db(frames=128, worker_threads=1, max_memory=4096) as database:
        database.sql("CREATE TABLE t(v INT PRIMARY KEY)")
        database.sql("INSERT INTO t VALUES (1), (2)")
        with pytest.raises(bb.ExecutionError):
            database.sql("SELECT (SELECT v FROM t)")
        stats = database.resource_stats()
        assert stats["active_worker_slots"] == 0
        assert stats["query_memory_used"] == 0
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(2,)]


def test_transaction_timeout_gc_rolls_back_and_clears_session() -> None:
    with bb.db(frames=128, transaction_timeout=0.01) as database, database.connect() as connection:
        database.sql("CREATE TABLE t(v INT)")
        connection.begin()
        connection.sql("INSERT INTO t VALUES (1)")
        threading.Event().wait(0.03)
        stats = connection.collect_garbage()
        assert stats["timed_out"] == 1
        assert not connection.in_transaction
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]


def test_database_gc_clears_the_timed_out_owner_and_releases_its_schema_lease() -> None:
    with bb.db(frames=128, transaction_timeout=0.01) as database, database.connect() as owner:
        database.sql("CREATE TABLE t(v INT)")
        owner.begin()
        owner.sql("INSERT INTO t VALUES (1)")
        threading.Event().wait(0.03)

        stats = database.collect_garbage()
        assert stats["timed_out"] == 1
        assert not owner.in_transaction
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]
        database.sql("DROP TABLE t")
