"""Database and connection lifecycle tests."""

from __future__ import annotations

from pathlib import Path

import pytest

import bumblebeedb as bb


@pytest.mark.durable
def test_durable_close_and_reopen(database_path: Path) -> None:
    with bb.db(database_path, frames=64) as database:
        database.sql("CREATE TABLE durable_values(value INTEGER PRIMARY KEY)")
        database.sql("INSERT INTO durable_values VALUES (7), (11)")

    with bb.db(database_path, frames=64) as reopened:
        assert reopened.sql("SELECT value FROM durable_values ORDER BY value").tuples() == [
            (7,),
            (11,),
        ]


def test_context_manager_closes_on_success_and_exception() -> None:
    with bb.db(frames=64) as database:
        assert not database.closed
    assert database.closed

    with pytest.raises(RuntimeError, match="sentinel"):
        with bb.db(frames=64) as failed:
            raise RuntimeError("sentinel")
    assert failed.closed


def test_close_is_idempotent_and_rejects_new_work() -> None:
    database = bb.db(frames=64)
    connection = database.connect()
    database.close()
    database.close()

    assert database.closed
    assert connection.closed
    with pytest.raises(bb.DatabaseClosedError):
        database.sql("SELECT 1")
    with pytest.raises(bb.DatabaseClosedError):
        database.connect()
    with pytest.raises(bb.DatabaseClosedError):
        connection.sql("SELECT 1")


def test_connection_context_closes_only_the_connection() -> None:
    database = bb.db(frames=64)
    with database.connect() as connection:
        assert connection.sql("SELECT 1").tuples() == [(1,)]
    assert connection.closed
    assert database.sql("SELECT 2").tuples() == [(2,)]
    database.close()


def test_database_sql_rejects_transaction_control() -> None:
    with bb.db(frames=64) as database:
        with pytest.raises(bb.ProgrammingError, match="connect"):
            database.sql("BEGIN")
