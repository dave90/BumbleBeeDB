"""Shared fixtures for the BumbleBeeDB Python test suite."""

from __future__ import annotations

from pathlib import Path

import pytest


@pytest.fixture
def database_path(tmp_path: Path) -> Path:
    """A fresh durable database path whose parent already exists."""

    return tmp_path / "database.bbdb"


@pytest.fixture
def external_table_dir(tmp_path: Path) -> Path:
    """A fresh directory for external Parquet table files."""

    path = tmp_path / "external"
    path.mkdir()
    return path


@pytest.fixture
def sql_file(tmp_path: Path) -> Path:
    """A writable UTF-8 SQL-file location for script tests."""

    return tmp_path / "script.sql"
