"""Run the shared sqllogictest corpus directly through the Python extension."""

from __future__ import annotations

import math
import shutil
import sys
import tempfile
import time
from datetime import date, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any

import bumblebeedb as bb


REPO_ROOT = Path(__file__).resolve().parents[3]
SHELL_RUNNER_DIRECTORY = REPO_ROOT / "test" / "e2e"
if str(SHELL_RUNNER_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(SHELL_RUNNER_DIRECTORY))

from slt_runner import SltError, SltSkip, _compare, parse_slt  # noqa: E402


def _truthy(value: str) -> bool:
    return value.lower() in {"1", "true", "yes", "on"}


def _database_options(config: dict[str, str]) -> dict[str, Any]:
    options: dict[str, Any] = {}
    for key, value in config.items():
        if key == "threads":
            options["worker_threads"] = int(value)
        elif key == "max_memory":
            options["max_memory"] = int(value)
        elif key == "morsel_pages":
            options["morsel_pages"] = int(value)
        elif key == "txn_timeout":
            options["transaction_timeout"] = int(value) / 1000.0
        elif key == "prefer_external":
            options["prefer_external"] = _truthy(value)
        else:
            raise SltError(f"unknown # config key: {key!r}")
    return options


def _seed_mock_tables(database: bb.Database) -> None:
    database.execute_script(
        """
        CREATE TABLE mock_ints_1(colA INT, colB INT);
        INSERT INTO mock_ints_1 VALUES (1, 100), (2, 200), (3, 300), (4, 400);
        CREATE TABLE mock_ints_2(colC INT, colD INT);
        INSERT INTO mock_ints_2 VALUES (1, 10), (2, 20), (3, 30);
        CREATE TABLE mock_people(id INT, name VARCHAR, age INT);
        INSERT INTO mock_people VALUES (1, 'Ada', 37), (2, 'Grace', 44);
        """
    )


def _format_float(value: float, logical_type: str | None) -> str:
    if math.isnan(value) or math.isinf(value):
        return str(value).lower()
    if logical_type == "FLOAT":
        return format(value, ".9g")
    rendered = repr(value)
    if rendered.endswith(".0"):
        rendered = rendered[:-2]
    return rendered


def _format_value(value: Any, logical_type: str | None = None) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return f"'{value}'"
    if isinstance(value, float):
        return _format_float(value, logical_type)
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, datetime):
        return value.isoformat(sep=" ")
    if isinstance(value, date):
        return value.isoformat()
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(_format_value(child) for child in value) + "]"
    return str(value)


def _result_rows(result: bb.Result) -> list[str]:
    if result.is_command:
        return [result.status]
    if result.affected_rows is not None and result.command_tag.lower() in {"insert", "update", "delete"}:
        return [str(result.affected_rows)]
    types = result.types
    return [
        " ".join(_format_value(value, types[column] if column < len(types) else None) for column, value in enumerate(row))
        for row in result.tuples()
    ]


class PythonSltSession:
    """One Database plus labeled sequential Connections for a corpus file."""

    def __init__(self, path: Path | None, options: dict[str, Any], seed_mock: bool):
        self._path = path
        self._options = options
        self._seed_mock = seed_mock
        self.database = bb.db(path, **options)
        self.connections: dict[str, bb.Connection] = {}
        if seed_mock:
            _seed_mock_tables(self.database)

    def connection(self, label: str) -> bb.Connection:
        connection = self.connections.get(label)
        if connection is None:
            connection = self.database.connect()
            self.connections[label] = connection
        return connection

    def run(self, record: Any) -> tuple[bool, str, list[str]]:
        connection = self.connection(record.session)
        sql = record.sql.strip()
        try:
            if sql == "\\gc":
                stats = connection.collect_garbage()
                rows = [f"GC: aborted {stats['timed_out']} timed-out transaction(s)"]
            elif sql.startswith("\\vacuum "):
                connection.vacuum(sql[len("\\vacuum ") :].strip())
                rows = []
            else:
                rows = _result_rows(connection.sql(sql))
            return True, "", rows
        except Exception as error:  # SQL exception classes are part of the outcome under test.
            return False, str(error), []

    def restart(self) -> None:
        if self._path is None:
            raise SltError("# restart requires a durable Python SLT run")
        self.close()
        self.database = bb.db(self._path, **self._options)
        self.connections = {}
        if self._seed_mock:
            _seed_mock_tables(self.database)

    def close(self) -> None:
        for connection in self.connections.values():
            try:
                connection.close()
            except Exception:
                pass
        self.connections.clear()
        try:
            self.database.close()
        except Exception:
            pass


def run_python_slt(path: Path, *, small_vector: bool, durable: bool) -> None:
    slt = parse_slt(path)
    if "small_vector" in slt.requires and not small_vector:
        raise SltSkip("requires a wheel built with BBDB_VECTOR_SIZE=4")
    if "durable" in slt.requires and not durable:
        raise SltSkip("requires BBDB_PY_SLT_DURABLE=1")
    if any(record.kind == "restart" for record in slt.records) and "durable" not in slt.requires:
        raise SltError(f"{path.name}: '# restart' needs '# require: durable'")

    with tempfile.TemporaryDirectory(prefix="bbdb_python_slt_") as directory:
        scratch = Path(directory)
        fixtures = REPO_ROOT / "test" / "e2e" / "fixtures"
        for fixture in slt.fixtures:
            shutil.copy(fixtures / fixture, scratch)
        for record in slt.records:
            if record.sql:
                record.sql = record.sql.replace("${TMPDIR}", str(scratch))

        db_path = scratch / "python.bbdb" if durable else None
        session = PythonSltSession(db_path, _database_options(slt.config), slt.seed_mock)
        try:
            for record in slt.records:
                if record.kind == "sleep":
                    time.sleep(record.sleep_ms / 1000.0)
                    continue
                if record.kind == "restart":
                    session.restart()
                    continue

                ok, message, rows = session.run(record)
                location = f"{path.name}:{record.line}"
                if record.kind == "statement":
                    if record.expect_error and ok:
                        raise SltError(f"{location}: statement expected an error but succeeded:\n  {record.sql}")
                    if not record.expect_error and not ok:
                        raise SltError(
                            f"{location}: statement failed unexpectedly:\n  {record.sql}\n  error: {message}"
                        )
                else:
                    if not ok:
                        raise SltError(f"{location}: query failed unexpectedly:\n  {record.sql}\n  error: {message}")
                    try:
                        _compare(rows, record.expected, record.sort)
                    except SltError as error:
                        raise SltError(f"{location}: {error}\n  SQL: {record.sql}") from None
        finally:
            session.close()
