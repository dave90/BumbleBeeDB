"""Parametrize the complete existing SQL corpus over the in-process Python API."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from slt_python_runner import REPO_ROOT, SltError, SltSkip, run_python_slt


SLT_FILES = sorted((REPO_ROOT / "test" / "e2e" / "slt").rglob("*.slt"))


def _enabled(name: str) -> bool:
    return os.environ.get(name, "0").lower() not in {"0", "", "false", "no"}


@pytest.mark.parametrize(
    "slt_file",
    SLT_FILES,
    ids=lambda path: str(path.relative_to(REPO_ROOT / "test" / "e2e" / "slt")),
)
def test_sql_corpus_through_python(slt_file: Path) -> None:
    try:
        run_python_slt(
            slt_file,
            small_vector=_enabled("BBDB_PY_SLT_SMALL_VECTOR"),
            durable=_enabled("BBDB_PY_SLT_DURABLE"),
        )
    except SltSkip as skip:
        pytest.skip(str(skip))
    except SltError as error:
        pytest.fail(str(error), pytrace=False)
