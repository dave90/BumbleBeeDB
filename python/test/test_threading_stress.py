"""Bounded reproducible mixed-workload stress and error-isolation tests."""

from __future__ import annotations

import random
from concurrent.futures import ThreadPoolExecutor

import pytest

import bumblebeedb as bb


@pytest.mark.concurrency
def test_reproducible_disjoint_update_and_read_stress() -> None:
    workers = 4
    iterations = 50
    with bb.db(frames=512, worker_threads=workers) as database:
        database.sql("CREATE TABLE counters(id INT PRIMARY KEY, value INT)")
        database.sql("INSERT INTO counters VALUES " + ",".join(f"({index}, 0)" for index in range(workers)))

        def workload(worker: int) -> None:
            randomizer = random.Random(7_000 + worker)
            for _ in range(iterations):
                database.sql(f"UPDATE counters SET value = value + 1 WHERE id = {worker}")
                probe = randomizer.randrange(workers)
                assert database.sql(f"SELECT value FROM counters WHERE id = {probe}").row_count == 1

        with ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(workload, range(workers)))

        assert database.sql("SELECT id, value FROM counters ORDER BY id").tuples() == [
            (index, iterations) for index in range(workers)
        ]
        assert database.sql("SELECT SUM(value) FROM counters").tuples() == [(workers * iterations,)]
        stats = database.resource_stats()
        assert stats["active_worker_slots"] == 0
        assert stats["query_memory_used"] == 0


@pytest.mark.concurrency
def test_failures_in_parallel_callers_do_not_poison_valid_queries() -> None:
    with bb.db(frames=256, worker_threads=4) as database:
        database.sql("CREATE TABLE values_table(id INT PRIMARY KEY)")
        database.sql("INSERT INTO values_table VALUES (1), (2)")

        def call(kind: str) -> str:
            try:
                if kind == "parser":
                    database.sql("SELEC 1")
                elif kind == "binder":
                    database.sql("SELECT * FROM absent")
                elif kind == "execution":
                    database.sql("SELECT (SELECT id FROM values_table)")
                elif kind == "conflict":
                    database.sql("INSERT INTO values_table VALUES (1)")
                else:
                    assert database.sql("SELECT SUM(id) FROM values_table").tuples() == [(3,)]
                    return "ok"
            except bb.BumbleBeeError:
                return kind
            raise AssertionError(f"{kind} unexpectedly succeeded")

        work = ["parser", "binder", "execution", "conflict"] + ["valid"] * 12
        with ThreadPoolExecutor(max_workers=8) as pool:
            outcomes = list(pool.map(call, work))
        assert outcomes.count("ok") == 12
        assert database.sql("SELECT SUM(id) FROM values_table").tuples() == [(3,)]
        assert database.resource_stats()["active_worker_slots"] == 0
