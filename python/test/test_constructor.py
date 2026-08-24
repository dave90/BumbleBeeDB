"""Construction and minimal package-surface tests."""

from __future__ import annotations

from pathlib import Path

import pytest

import bumblebeedb as bb


def test_import_version_and_default_constructor() -> None:
    assert bb.__version__ == "0.1.0"
    database = bb.db(frames=64)
    try:
        assert database.sql("SELECT 42 AS answer").tuples() == [(42,)]
    finally:
        database.close()


def test_database_class_accepts_pathlike(database_path: Path) -> None:
    database = bb.Database(database_path, frames=64)
    database.close()
    assert database_path.exists()


@pytest.mark.parametrize(
    ("keyword", "value"),
    [
        ("worker_threads", True),
        ("worker_threads", -1),
        ("max_memory", 0),
        ("max_memory", -1),
        ("frames", 0),
        ("frames", -1),
        ("morsel_pages", 0),
        ("morsel_pages", -1),
        ("transaction_timeout", 0),
        ("transaction_timeout", float("inf")),
        ("prefer_external", 1),
    ],
)
def test_invalid_constructor_values(keyword: str, value: object) -> None:
    options = {"frames": 64}
    options[keyword] = value
    with pytest.raises(ValueError):
        bb.db(**options)


def test_invalid_constructor_path_and_unknown_option() -> None:
    with pytest.raises(ValueError):
        bb.db("")
    with pytest.raises(TypeError):
        bb.db(b"database.bbdb")
    with pytest.raises(TypeError):
        bb.db(unknown_option=True)
