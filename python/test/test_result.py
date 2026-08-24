"""Minimal owning Result tests."""

from __future__ import annotations

import pytest

import bumblebeedb as bb


def test_result_metadata_basic_tuples_and_repeatability() -> None:
    database = bb.db(frames=64)
    result = database.sql("SELECT 42 AS answer, 'hello' AS greeting, NULL AS missing")

    assert result.columns == ["answer", "greeting", "missing"]
    assert result.types == ["INTEGER", "VARCHAR", "UNKNOWN"]
    assert result.row_count == 1
    assert len(result) == 1
    assert not result.is_command
    assert result.command_tag == ""
    assert result.status == ""
    assert result.affected_rows is None
    assert result.tuples() == [(42, "hello", None)]
    assert result.tuples() == [(42, "hello", None)]

    database.close()
    assert result.tuples() == [(42, "hello", None)]


def test_command_and_affected_row_metadata() -> None:
    with bb.db(frames=64) as database:
        create = database.sql("CREATE TABLE metadata_test(value INTEGER PRIMARY KEY)")
        insert = database.sql("INSERT INTO metadata_test VALUES (1), (2)")

        assert create.is_command
        assert create.command_tag == "CREATE TABLE"
        assert create.status
        assert insert.command_tag == "Insert"
        assert insert.affected_rows == 2


def test_cursor_fetch_and_iteration_are_separate_from_full_conversion() -> None:
    with bb.db(frames=64) as database:
        result = database.sql("VALUES (1), (2), (3), (4)")

        assert result.fetchmany(0) == []
        assert result.fetchone() == (1,)
        assert result.fetchmany() == [(2,)]
        assert result.fetchmany(2) == [(3,), (4,)]
        assert result.fetchone() is None
        assert result.fetchall() == []
        assert result.tuples() == [(1,), (2,), (3,), (4,)]

        iterated = database.sql("VALUES (5), (6), (7)")
        assert iter(iterated) is iterated
        assert next(iterated) == (5,)
        assert list(iterated) == [(6,), (7,)]
        with pytest.raises(StopIteration):
            next(iterated)


def test_fetchmany_rejects_negative_size() -> None:
    with bb.db(frames=64) as database:
        with pytest.raises(ValueError):
            database.sql("SELECT 1").fetchmany(-1)
