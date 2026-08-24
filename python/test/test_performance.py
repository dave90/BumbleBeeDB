"""Loose performance guardrails for native staging versus unavoidable Python boxing."""

from __future__ import annotations

import time

import numpy as np
import pandas as pd
import pytest

import bumblebeedb as bb


@pytest.mark.slow
def test_result_staging_boxing_and_constant_time_metadata_guardrails() -> None:
    rows = 75_000
    with bb.db(frames=1024, worker_threads=2) as database:
        database.load_df(pd.DataFrame({"value": np.arange(rows, dtype=np.int64)}), "benchmark_rows")

        started = time.perf_counter()
        result = database.sql("SELECT _id, value FROM benchmark_rows")
        native_seconds = time.perf_counter() - started

        started = time.perf_counter()
        tuples = result.tuples()
        boxing_seconds = time.perf_counter() - started

        started = time.perf_counter()
        frame = result.to_df()
        dataframe_seconds = time.perf_counter() - started

        started = time.perf_counter()
        for _ in range(100_000):
            assert len(result) == rows and result.row_count == rows
        metadata_seconds = time.perf_counter() - started

    assert tuples[0] == (0, 0) and tuples[-1] == (rows - 1, rows - 1)
    assert len(frame) == rows
    # These generous ceilings catch accidental quadratic work or blocking hangs without turning a
    # shared CI runner into a microbenchmark. Native staging and Python boxing are timed separately.
    assert native_seconds < 15
    assert boxing_seconds < 15
    assert dataframe_seconds < 15
    assert metadata_seconds < 3
