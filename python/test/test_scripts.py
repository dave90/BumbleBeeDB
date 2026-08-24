"""SQL script and UTF-8 file execution tests."""

from __future__ import annotations

from pathlib import Path

import pytest

import bumblebeedb as bb


def test_execute_script_returns_results_in_statement_order() -> None:
    with bb.db() as database:
        results = database.execute_script(
            "CREATE TABLE t(v INT); INSERT INTO t VALUES (1), (2); SELECT v FROM t ORDER BY v;"
        )

        assert [result.command_tag.upper() for result in results[:2]] == ["CREATE TABLE", "INSERT"]
        assert results[2].tuples() == [(1,), (2,)]


def test_run_file_accepts_pathlike_multiple_statements_and_utf8(sql_file: Path) -> None:
    sql_file.write_text(
        "CREATE TABLE café(label VARCHAR);\n"
        "INSERT INTO café VALUES ('Grüezi 🐝');\n"
        "SELECT label FROM café;\n",
        encoding="utf-8",
    )

    with bb.db() as database:
        results = database.run_file(sql_file)
        assert results[-1].tuples() == [("Grüezi 🐝",)]


def test_run_file_reports_missing_directory_and_unreadable_files(sql_file: Path, tmp_path: Path) -> None:
    with bb.db() as database:
        with pytest.raises(OSError):
            database.run_file(tmp_path / "missing.sql")
        with pytest.raises(OSError):
            database.run_file(tmp_path)

        sql_file.write_text("SELECT 1", encoding="utf-8")
        sql_file.chmod(0)
        try:
            with pytest.raises(OSError):
                database.run_file(sql_file)
        finally:
            sql_file.chmod(0o600)


def test_run_file_rejects_non_utf8(sql_file: Path) -> None:
    sql_file.write_bytes(b"SELECT '\xff';")
    with bb.db() as database, pytest.raises(UnicodeDecodeError):
        database.run_file(sql_file)


def test_script_error_has_statement_index_and_stops_at_first_failure() -> None:
    with bb.db() as database:
        with pytest.raises(bb.BinderError, match=r"script statement 2"):
            database.execute_script(
                "CREATE TABLE t(v INT); INSERT INTO missing VALUES (1); INSERT INTO t VALUES (3);"
            )
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]

        with pytest.raises(bb.ParserError, match=r"script statement 2"):
            database.execute_script("SELECT 1; SELEC 2; SELECT 3")


def test_script_explicit_transactions_commit_and_rollback() -> None:
    with bb.db() as database:
        database.sql("CREATE TABLE t(v INT)")
        database.execute_script("BEGIN; INSERT INTO t VALUES (1); COMMIT;")
        database.execute_script("BEGIN; INSERT INTO t VALUES (2); ROLLBACK;")
        assert database.sql("SELECT v FROM t").tuples() == [(1,)]


def test_script_rolls_back_a_transaction_left_open_at_end() -> None:
    with bb.db() as database:
        database.sql("CREATE TABLE t(v INT)")
        with pytest.raises(bb.ProgrammingError, match="left open.*rolled back"):
            database.execute_script("BEGIN; INSERT INTO t VALUES (1)")
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(0,)]


def test_connection_script_can_continue_a_preexisting_transaction() -> None:
    with bb.db() as database, database.connect() as connection:
        database.sql("CREATE TABLE t(v INT)")
        connection.begin()
        connection.execute_script("INSERT INTO t VALUES (1); SELECT COUNT(*) FROM t")
        assert connection.in_transaction
        connection.commit()
        assert database.sql("SELECT COUNT(*) FROM t").tuples() == [(1,)]


def test_public_native_types_have_docstrings() -> None:
    assert bb.Database.__doc__
    assert bb.Connection.sql.__doc__
    assert bb.Result.to_df.__doc__
