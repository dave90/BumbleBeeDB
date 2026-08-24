"""True parallel write and DDL tests through one shared Database."""

from __future__ import annotations

import threading
from concurrent.futures import ThreadPoolExecutor

import pytest

import bumblebeedb as bb


@pytest.mark.concurrency
def test_many_disjoint_writers_preserve_rows_and_generated_ids() -> None:
    writers = 8
    rows_per_writer = 25
    with bb.db(frames=512, worker_threads=4) as database:
        database.sql("CREATE TABLE events(value INT)")

        def write_partition(partition: int) -> None:
            for offset in range(rows_per_writer):
                value = partition * rows_per_writer + offset
                database.sql(f"INSERT INTO events VALUES ({value})")

        with ThreadPoolExecutor(max_workers=writers) as pool:
            list(pool.map(write_partition, range(writers)))

        rows = database.sql("SELECT _id, value FROM events ORDER BY value").tuples()
        assert len(rows) == writers * rows_per_writer
        assert len({row[0] for row in rows}) == len(rows)
        assert [row[1] for row in rows] == list(range(writers * rows_per_writer))


@pytest.mark.concurrency
def test_duplicate_primary_key_race_has_one_winner() -> None:
    with bb.db(frames=128, worker_threads=2) as database:
        database.sql("CREATE TABLE unique_values(id INT PRIMARY KEY, payload INT)")
        start = threading.Barrier(2)

        def insert(payload: int) -> str:
            start.wait(timeout=5)
            try:
                database.sql(f"INSERT INTO unique_values VALUES (1, {payload})")
                return "committed"
            except bb.ConflictError:
                return "conflict"

        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(insert, (10, 20)))
        assert sorted(outcomes) == ["committed", "conflict"]
        assert database.sql("SELECT id, payload FROM unique_values").row_count == 1


@pytest.mark.concurrency
def test_same_row_transaction_race_has_one_valid_winner() -> None:
    with bb.db(frames=128, worker_threads=2) as database:
        database.sql("CREATE TABLE counters(id INT PRIMARY KEY, value INT)")
        database.sql("INSERT INTO counters VALUES (1, 0)")
        snapshots_ready = threading.Barrier(2)

        def increment() -> str:
            with database.connect() as connection:
                connection.begin()
                assert connection.sql("SELECT value FROM counters WHERE id = 1").tuples() == [(0,)]
                snapshots_ready.wait(timeout=5)
                try:
                    connection.sql("UPDATE counters SET value = value + 1 WHERE id = 1")
                    connection.commit()
                    return "committed"
                except bb.ConflictError:
                    return "conflict"

        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(lambda _: increment(), range(2)))
        assert sorted(outcomes) == ["committed", "conflict"]
        assert database.sql("SELECT value FROM counters WHERE id = 1").tuples() == [(1,)]


@pytest.mark.concurrency
def test_concurrent_distinct_and_same_name_ddl() -> None:
    with bb.db(frames=128, worker_threads=4) as database:
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda index: database.sql(f"CREATE TABLE table_{index}(v INT)"), range(4)))
        assert [table["name"] for table in database.get_tables()] == [f"table_{index}" for index in range(4)]

        start = threading.Barrier(2)

        def create_same(index: int) -> str:
            start.wait(timeout=5)
            try:
                database.sql(f"CREATE TABLE one_name(v{index} INT)")
                return "created"
            except bb.DatabaseError:
                return "exists"

        with ThreadPoolExecutor(max_workers=2) as pool:
            assert sorted(pool.map(create_same, range(2))) == ["created", "exists"]
        assert database.describe_table("one_name")["name"] == "one_name"
