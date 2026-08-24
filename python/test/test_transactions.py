"""Explicit Connection transaction API tests."""

from __future__ import annotations

import threading

import pytest

import bumblebeedb as bb


def test_begin_commit_and_rollback_methods() -> None:
    with bb.db() as database, database.connect() as connection:
        database.sql("CREATE TABLE t(v INT)")
        connection.begin()
        connection.sql("INSERT INTO t VALUES (1)")
        connection.commit()
        assert database.sql("SELECT v FROM t").tuples() == [(1,)]

        connection.begin("serializable")
        connection.sql("INSERT INTO t VALUES (2)")
        connection.rollback()
        assert database.sql("SELECT v FROM t").tuples() == [(1,)]


def test_transaction_api_misuse_is_a_programming_error() -> None:
    with bb.db() as database, database.connect() as connection:
        with pytest.raises(ValueError):
            connection.begin("read_committed")
        with pytest.raises(bb.ProgrammingError, match="no transaction"):
            connection.commit()
        with pytest.raises(bb.ProgrammingError, match="no transaction"):
            connection.rollback()
        connection.begin()
        with pytest.raises(bb.ProgrammingError, match="already in progress"):
            connection.begin()
        connection.rollback()


def test_transaction_context_commits_and_rolls_back_python_exception() -> None:
    with bb.db() as database, database.connect() as connection:
        database.sql("CREATE TABLE t(v INT)")
        with connection.transaction() as active:
            assert active is connection
            active.sql("INSERT INTO t VALUES (1)")

        with pytest.raises(RuntimeError, match="application failure"):
            with connection.transaction("serializable"):
                connection.sql("INSERT INTO t VALUES (2)")
                raise RuntimeError("application failure")

        assert database.sql("SELECT v FROM t").tuples() == [(1,)]


def test_snapshot_visibility_across_independent_connections() -> None:
    with bb.db() as database, database.connect() as first, database.connect() as second:
        database.sql("CREATE TABLE t(v INT)")
        first.begin()
        assert first.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]

        second.begin()
        second.sql("INSERT INTO t VALUES (1)")
        assert first.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]
        second.commit()
        assert first.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]
        first.commit()

        first.begin()
        assert first.sql("SELECT COUNT(*) FROM t").tuples() == [(1,)]
        first.commit()


def test_statement_error_aborts_explicit_transaction() -> None:
    with bb.db() as database, database.connect() as connection:
        database.sql("CREATE TABLE t(id INT PRIMARY KEY)")
        connection.begin()
        connection.sql("INSERT INTO t VALUES (1)")
        with pytest.raises(bb.ConflictError):
            connection.sql("INSERT INTO t VALUES (1)")
        assert not connection.in_transaction
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]


def test_ddl_is_rejected_and_connection_close_rolls_back() -> None:
    with bb.db() as database:
        connection = database.connect()
        connection.begin()
        with pytest.raises(bb.ProgrammingError, match="DDL"):
            connection.sql("CREATE TABLE forbidden(v INT)")
        connection.rollback()

        database.sql("CREATE TABLE t(v INT)")
        connection.begin()
        connection.sql("INSERT INTO t VALUES (1)")
        connection.close()
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]


def test_serializable_write_skew_rejects_one_writer_and_read_only_commits() -> None:
    with bb.db() as database, database.connect() as first, database.connect() as second:
        database.sql("CREATE TABLE doctors(id INT PRIMARY KEY, on_call INT)")
        database.sql("INSERT INTO doctors VALUES (1, 1), (2, 1)")
        first.begin("serializable")
        second.begin("serializable")
        assert first.sql("SELECT COUNT(*) FROM doctors WHERE on_call = 1").tuples() == [(2,)]
        assert second.sql("SELECT COUNT(*) FROM doctors WHERE on_call = 1").tuples() == [(2,)]
        first.sql("UPDATE doctors SET on_call = 0 WHERE id = 1")
        second.sql("UPDATE doctors SET on_call = 0 WHERE id = 2")
        first.commit()
        with pytest.raises(bb.ConflictError, match="serializable"):
            second.commit()
        assert database.sql("SELECT SUM(on_call) FROM doctors").tuples() == [(1,)]

        first.begin("serializable")
        assert first.sql("SELECT COUNT(*) FROM doctors").tuples() == [(2,)]
        first.commit()


@pytest.mark.concurrency
def test_one_connection_rejects_concurrent_statement_use() -> None:
    with bb.db(frames=512, worker_threads=2) as database, database.connect() as connection:
        database.sql("CREATE TABLE numbers(value INT PRIMARY KEY)")
        database.sql("INSERT INTO numbers VALUES " + ",".join(f"({value})" for value in range(500)))
        query = threading.Thread(
            target=lambda: connection.sql(
                "SELECT SUM(a.value + b.value + c.value) FROM numbers a, numbers b, numbers c"
            )
        )
        query.start()
        while database.resource_stats()["active_operations"] == 0:
            pass
        with pytest.raises(bb.ProgrammingError, match="concurrent statement"):
            connection.sql("SELECT 1")
        query.join(timeout=10)
        assert not query.is_alive()
