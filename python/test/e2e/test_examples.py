"""Execute every public Python example as documentation regression coverage."""

from __future__ import annotations

import runpy
from pathlib import Path

import pytest


EXAMPLE_DIRECTORY = Path(__file__).resolve().parents[3] / "examples" / "python"
EXAMPLES = sorted(EXAMPLE_DIRECTORY.glob("[0-9][0-9]_*.py"))


@pytest.mark.parametrize("example", EXAMPLES, ids=lambda path: path.stem)
def test_python_example(example: Path) -> None:
    runpy.run_path(str(example), run_name="__main__")


def test_all_required_examples_are_present() -> None:
    assert [path.name for path in EXAMPLES] == [
        "01_getting_started.py",
        "02_persistent_database.py",
        "03_dataframe_analytics.py",
        "04_transactions.py",
        "05_concurrent_queries.py",
        "06_external_parquet.py",
        "07_null_and_type_handling.py",
        "08_query_plans_and_scripts.py",
    ]
