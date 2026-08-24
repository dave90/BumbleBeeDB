"""Transactional pandas DataFrame ingestion tests."""

from __future__ import annotations

import gc
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import weakref

import numpy as np
import pandas as pd
import pytest

import bumblebeedb as bb


def test_basic_load_generates_id_and_copies_input() -> None:
    frame = pd.DataFrame({"name": ["bee", "hive"], "value": [7, 11]})
    frame_reference = weakref.ref(frame)
    with bb.db(frames=128) as database:
        assert database.load_df(frame, "facts") is None
        frame.iloc[0, 0] = "mutated"
        frame.iloc[0, 1] = 999
        del frame
        gc.collect()
        assert frame_reference() is None

        result = database.sql("SELECT _id, name, value FROM facts ORDER BY _id")
        assert result.tuples() == [(0, "bee", 7), (1, "hive", 11)]


def test_explicit_and_composite_primary_keys() -> None:
    with bb.db(frames=128) as database:
        database.load_df(
            pd.DataFrame({"region": [1, 1, 2], "code": [10, 20, 10], "value": [10, 20, 30]}),
            "items",
            primary_key=["region", "code"],
        )
        assert database.sql("SELECT region, code, value FROM items ORDER BY region, code").tuples() == [
            (1, 10, 10),
            (1, 20, 20),
            (2, 10, 30),
        ]


def test_create_duplicate_key_is_cleanup_safe() -> None:
    with bb.db(frames=128) as database:
        with pytest.raises(bb.ConflictError):
            database.load_df(
                pd.DataFrame({"id": [1, 1], "value": ["first", "duplicate"]}),
                "failed_load",
                primary_key="id",
            )
        with pytest.raises(bb.BinderError):
            database.sql("SELECT * FROM failed_load")


def test_append_is_atomic_and_requires_exact_schema() -> None:
    with bb.db(frames=128) as database:
        database.load_df(
            pd.DataFrame({"id": pd.Series([1, 2], dtype="int64"), "value": ["a", "b"]}),
            "append_target",
            primary_key="id",
        )

        with pytest.raises(bb.ConflictError):
            database.load_df(
                pd.DataFrame({"id": pd.Series([3, 2], dtype="int64"), "value": ["c", "duplicate"]}),
                "append_target",
                primary_key="id",
                if_exists="append",
            )
        assert database.sql("SELECT * FROM append_target ORDER BY id").tuples() == [(1, "a"), (2, "b")]

        with pytest.raises(bb.DataError, match="schema"):
            database.load_df(
                pd.DataFrame({"id": pd.Series([3], dtype="int32"), "value": ["c"]}),
                "append_target",
                if_exists="append",
            )
        with pytest.raises(bb.DataError, match="schema"):
            database.load_df(
                pd.DataFrame({"value": ["c"], "id": pd.Series([3], dtype="int64")}),
                "append_target",
                if_exists="append",
            )
        assert database.sql("SELECT count(*) FROM append_target").tuples() == [(2,)]

        database.load_df(
            pd.DataFrame({"id": pd.Series([3], dtype="int64"), "value": ["c"]}),
            "append_target",
            primary_key="id",
            if_exists="append",
        )
        assert database.sql("SELECT * FROM append_target ORDER BY id").tuples() == [
            (1, "a"),
            (2, "b"),
            (3, "c"),
        ]


def test_if_exists_error_and_append_creates_when_missing() -> None:
    frame = pd.DataFrame({"value": [1]})
    with bb.db(frames=64) as database:
        database.load_df(frame, "existing")
        with pytest.raises(bb.DataError, match="already exists"):
            database.load_df(frame, "existing")

        database.load_df(frame, "created_by_append", if_exists="append")
        assert database.sql("SELECT value FROM created_by_append").tuples() == [(1,)]


def test_empty_frame_creates_typed_empty_table_and_zero_columns_fail() -> None:
    with bb.db(frames=64) as database:
        database.load_df(
            pd.DataFrame(
                {
                    "id": pd.Series([], dtype="int64"),
                    "label": pd.Series([], dtype="string"),
                }
            ),
            "empty_table",
            primary_key="id",
        )
        result = database.sql("SELECT * FROM empty_table")
        assert result.types == ["BIGINT", "VARCHAR"]
        assert result.tuples() == []

        with pytest.raises(bb.DataError, match="zero columns"):
            database.load_df(pd.DataFrame(index=range(3)), "no_columns")


def test_include_named_and_unnamed_index() -> None:
    with bb.db(frames=64) as database:
        named = pd.DataFrame({"value": [10, 20]}, index=pd.Index([5, 7], name="source_id"))
        database.load_df(named, "with_named_index", include_index=True, primary_key="source_id")
        assert database.sql("SELECT * FROM with_named_index ORDER BY source_id").tuples() == [
            (5, 10),
            (7, 20),
        ]

        unnamed = pd.DataFrame({"value": [30]}, index=pd.Index([9]))
        database.load_df(unnamed, "with_index", include_index=True)
        assert database.sql('SELECT "index", value FROM with_index').tuples() == [(9, 30)]


def test_noncontiguous_and_sliced_numeric_views() -> None:
    base = np.arange(4000, dtype=np.int64)
    view = base[101:3101:3]
    assert not view.flags.c_contiguous
    frame = pd.DataFrame({"value": view}, copy=False)

    with bb.db(frames=128) as database:
        database.load_df(frame, "views")
        assert database.sql("SELECT count(*), min(value), max(value) FROM views").tuples() == [
            (1000, 101, 3098),
        ]


@pytest.mark.durable
def test_durable_loaded_data_survives_reopen(database_path: Path) -> None:
    frame = pd.DataFrame({"id": [1, 2], "label": ["persisted", "also persisted"]})
    with bb.db(database_path, frames=128) as database:
        database.load_df(frame, "durable_frame", primary_key="id")

    with bb.db(database_path, frames=128) as reopened:
        assert reopened.sql("SELECT * FROM durable_frame ORDER BY id").tuples() == [
            (1, "persisted"),
            (2, "also persisted"),
        ]


def test_table_and_column_names_are_preserved_and_quotable() -> None:
    with bb.db(frames=64) as database:
        database.load_df(pd.DataFrame({"select": [1], "spaced name": ["ok"]}), "odd table")
        assert database.sql('SELECT "select", "spaced name" FROM "odd table"').tuples() == [(1, "ok")]


def test_invalid_dataframe_arguments() -> None:
    with bb.db(frames=64) as database:
        with pytest.raises(TypeError):
            database.load_df({"value": [1]}, "not_a_frame")
        with pytest.raises(ValueError):
            database.load_df(pd.DataFrame({"value": [1]}), "bad_mode", if_exists="replace")
        with pytest.raises(TypeError):
            database.load_df(pd.DataFrame({"value": [1]}), "bad_pk", primary_key=[1])
        with pytest.raises(bb.DataError, match="not in the DataFrame"):
            database.load_df(pd.DataFrame({"value": [1]}), "bad_pk_name", primary_key="missing")
        with pytest.raises(bb.DataError, match="contains NULL"):
            database.load_df(pd.DataFrame({"id": pd.Series([1, None], dtype="Int64")}), "null_pk", primary_key="id")

        duplicate = pd.DataFrame([[1, 2]], columns=["same", "same"])
        with pytest.raises(bb.DataError, match="duplicate"):
            database.load_df(duplicate, "duplicates")

        with pytest.raises(bb.DataError, match="reserved"):
            database.load_df(pd.DataFrame({"_id": [1]}), "reserved")
        with pytest.raises(bb.DataError, match="strings"):
            database.load_df(pd.DataFrame([[1]], columns=[7]), "non_string_column")
        with pytest.raises(bb.DataError, match="variable-length"):
            database.load_df(pd.DataFrame({"key": ["a"]}), "string_pk", primary_key="key")


def test_loaded_table_participates_in_relational_and_dml_operations() -> None:
    with bb.db(frames=128) as database:
        database.load_df(pd.DataFrame({"id": [1, 2, 3], "group_id": [10, 10, 20]}), "facts", primary_key="id")
        database.load_df(pd.DataFrame({"group_id": [10, 20], "label": ["a", "b"]}), "groups", primary_key="group_id")

        assert database.sql(
            "SELECT groups.label, sum(facts.id) FROM facts "
            "JOIN groups ON facts.group_id = groups.group_id GROUP BY groups.label ORDER BY groups.label"
        ).tuples() == [("a", 3), ("b", 3)]
        assert database.sql("UPDATE facts SET group_id = 20 WHERE id = 2").affected_rows == 1
        assert database.sql("DELETE FROM facts WHERE id = 1").affected_rows == 1
        assert database.sql("SELECT id, group_id FROM facts ORDER BY id").tuples() == [(2, 20), (3, 20)]


@pytest.mark.concurrency
def test_concurrent_loads_into_distinct_and_same_tables() -> None:
    with bb.db(frames=512, worker_threads=2) as database:
        def load_distinct(table: str) -> None:
            database.load_df(pd.DataFrame({"value": np.arange(5_000, dtype=np.int64)}), table)

        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(load_distinct, ("left_load", "right_load")))
        assert database.sql("SELECT count(*) FROM left_load").tuples() == [(5_000,)]
        assert database.sql("SELECT count(*) FROM right_load").tuples() == [(5_000,)]

        def race_create(value: int) -> str:
            try:
                database.load_df(pd.DataFrame({"value": [value]}), "one_winner")
                return "created"
            except bb.DataError:
                return "exists"

        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(race_create, (1, 2)))
        assert sorted(outcomes) == ["created", "exists"]
        assert database.sql("SELECT count(*) FROM one_winner").tuples() == [(1,)]
