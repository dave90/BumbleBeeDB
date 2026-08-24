"""Catalog, safe table-name, explain, and vacuum helper tests."""

from __future__ import annotations

from pathlib import Path

import pytest

import bumblebeedb as bb


def _sql_path(path: Path) -> str:
    return str(path).replace("'", "''")


def test_get_tables_and_describe_table_are_structured_and_sorted() -> None:
    with bb.db() as database:
        assert database.get_tables() == []
        database.sql("CREATE TABLE zed(value VARCHAR)")
        database.sql("CREATE TABLE accounts(id BIGINT PRIMARY KEY, active BOOLEAN)")

        tables = database.get_tables()
        assert [table["name"] for table in tables] == ["accounts", "zed"]
        accounts = database.describe_table("accounts")
        assert accounts == {
            "name": "accounts",
            "columns": [
                {"name": "id", "type": "BIGINT", "primary_key": True},
                {"name": "active", "type": "BOOLEAN", "primary_key": False},
            ],
            "primary_key": ["id"],
            "generated_id": False,
            "storage": "row",
            "location": None,
            "estimated_rows": 0,
        }

        generated = database.describe_table("zed")
        assert generated["generated_id"] is True
        assert generated["primary_key"] == ["_id"]
        assert generated["columns"][0]["name"] == "_id"


def test_get_table_quotes_arbitrary_identifier_without_injection() -> None:
    name = 'odd " table; DROP TABLE sentinel; --'
    quoted = name.replace('"', '""')
    with bb.db() as database:
        database.sql("CREATE TABLE sentinel(v INT)")
        database.sql(f'CREATE TABLE "{quoted}"(v INT)')
        database.sql(f'INSERT INTO "{quoted}" VALUES (42)')

        assert database.get_table(name).tuples() == [(0, 42)]
        assert database.describe_table("sentinel")["name"] == "sentinel"


def test_remove_table_missing_policy_and_quoted_name() -> None:
    with bb.db() as database:
        database.sql('CREATE TABLE "to drop"(v INT)')
        assert database.remove_table("to drop") is True
        with pytest.raises(bb.DatabaseError):
            database.remove_table("to drop")
        assert database.remove_table("to drop", if_exists=True) is False


@pytest.mark.parametrize("mode", ["binder", "planner", "optimizer", "physical", "pipelines", "analyze"])
def test_explain_modes(mode: str) -> None:
    with bb.db() as database:
        database.sql("CREATE TABLE t(v INT)")
        plan = database.explain("SELECT * FROM t", mode=mode)
        assert f"=== {mode.upper()} ===" in plan


def test_explain_rejects_unknown_mode_before_execution() -> None:
    with bb.db() as database, pytest.raises(ValueError, match="mode must be"):
        database.explain("SELECT 1", mode="magic")


def test_vacuum_external_table_and_row_table_error(external_table_dir: Path) -> None:
    location = _sql_path(external_table_dir)
    with bb.db() as database:
        database.sql(
            f"CREATE TABLE ext(v INT) WITH (format='parquet', location='{location}')"
        )
        database.sql("INSERT INTO ext VALUES (1), (2)")
        database.sql("INSERT INTO ext VALUES (3)")
        orphan = external_table_dir / "orphan.parquet"
        orphan.write_bytes(b"not a live parquet file")

        removed = database.vacuum("ext")
        assert removed >= 1
        assert not orphan.exists()
        assert database.sql("SELECT v FROM ext ORDER BY v").tuples() == [(1,), (2,), (3,)]

        metadata = database.describe_table("ext")
        assert metadata["storage"] == "parquet"
        assert metadata["location"] == str(external_table_dir)
        assert metadata["estimated_rows"] == 3

        database.sql("CREATE TABLE heap(v INT)")
        with pytest.raises(bb.DatabaseError, match="only applies to external parquet"):
            database.vacuum("heap")


def test_connection_exposes_the_same_helpers() -> None:
    with bb.db() as database, database.connect() as connection:
        connection.sql("CREATE TABLE t(v INT)")
        assert connection.get_table("t").tuples() == []
        assert connection.get_tables()[0]["name"] == "t"
        assert "=== PHYSICAL ===" in connection.explain("SELECT * FROM t")
        assert connection.remove_table("t") is True
