"""pytest integration for the `.slt` end-to-end SQL suite.

Every ``*.slt`` file under ``test/e2e/slt/`` is collected as a single test, whose node id is
the file path (so the category subfolders — aggregates/, joins/, … — show up directly in the
test id and ``-k`` filters). The BumbleBee binary is selected via the ``BBDB_SLT_BIN`` env var,
defaulting to ``build/BumbleBee`` at the repo root, so the same corpus can run against the
default and small-vector builds.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from slt_runner import SltError, SltSkip, run_slt_file

# Repo root is two levels up from this file (test/e2e/conftest.py).
_REPO_ROOT = Path(__file__).resolve().parents[2]


def _runner_binary() -> str:
    override = os.environ.get("BBDB_SLT_BIN")
    if override:
        return override
    return str(_REPO_ROOT / "build" / "BumbleBee")


def _is_small_vector_build() -> bool:
    return os.environ.get("BBDB_SLT_SMALL_VECTOR", "0") not in ("0", "", "false", "no")


def pytest_collect_file(parent, file_path: Path):
    if file_path.suffix == ".slt":
        return SltCollector.from_parent(parent, path=file_path)
    return None


class SltCollector(pytest.File):
    def collect(self):
        yield SltItem.from_parent(self, name=self.path.stem)


class SltItem(pytest.Item):
    def runtest(self) -> None:
        try:
            run_slt_file(self.path, _runner_binary(), _is_small_vector_build())
        except SltSkip as skip:
            pytest.skip(str(skip))

    def repr_failure(self, excinfo):
        if isinstance(excinfo.value, SltError):
            # Show the harness's own readable message instead of a Python traceback.
            return str(excinfo.value)
        return super().repr_failure(excinfo)

    def reportinfo(self):
        return self.path, 0, f"slt: {self.path.relative_to(_REPO_ROOT)}"
