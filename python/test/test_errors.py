"""Native-to-Python exception hierarchy tests."""

from __future__ import annotations

import pytest

import bumblebeedb as bb


def test_sql_stage_exception_mapping() -> None:
    with bb.db(frames=64) as database:
        with pytest.raises(bb.ParserError):
            database.sql("SELEC 1")
        with pytest.raises(bb.BinderError):
            database.sql("SELECT * FROM missing_table")
        with pytest.raises(bb.PlannerError):
            database.sql("SELECT mystery_function(1)")
        with pytest.raises(bb.NotImplementedError):
            database.sql("SELECT 1 OFFSET 1")

        database.sql("CREATE TABLE scalar_values(value INTEGER PRIMARY KEY)")
        database.sql("INSERT INTO scalar_values VALUES (1), (2)")
        with pytest.raises(bb.ExecutionError, match="more than one row"):
            database.sql("SELECT (SELECT value FROM scalar_values)")
        with pytest.raises(bb.ConflictError):
            database.sql("INSERT INTO scalar_values VALUES (1)")


def test_exception_inheritance() -> None:
    assert issubclass(bb.DatabaseClosedError, bb.DatabaseError)
    assert issubclass(bb.ConflictError, bb.ExecutionError)
    assert issubclass(bb.ExecutionError, bb.DatabaseError)
    assert issubclass(bb.DatabaseError, bb.BumbleBeeError)
    assert issubclass(bb.ProgrammingError, bb.BumbleBeeError)
    assert issubclass(bb.DataError, bb.BumbleBeeError)
