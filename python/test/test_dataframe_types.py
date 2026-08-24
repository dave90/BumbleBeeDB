"""pandas dtype inference and scalar conversion tests."""

from __future__ import annotations

from datetime import date, datetime
from decimal import Decimal

import numpy as np
import pandas as pd
import pytest

import bumblebeedb as bb


def test_all_supported_numpy_and_nullable_dtypes() -> None:
    frame = pd.DataFrame(
        {
            "i8": pd.Series([-8, 8], dtype="int8"),
            "i16": pd.Series([-16, 16], dtype="int16"),
            "i32": pd.Series([-32, 32], dtype="int32"),
            "i64": pd.Series([-64, 64], dtype="int64"),
            "u8": pd.Series([8, 9], dtype="uint8"),
            "u16": pd.Series([16, 17], dtype="uint16"),
            "u32": pd.Series([32, 33], dtype="uint32"),
            "u64": pd.Series([2**63, 2**63 + 1], dtype="uint64"),
            "ni8": pd.Series([1, None], dtype="Int8"),
            "ni16": pd.Series([2, None], dtype="Int16"),
            "ni32": pd.Series([3, None], dtype="Int32"),
            "ni64": pd.Series([4, None], dtype="Int64"),
            "nu8": pd.Series([5, None], dtype="UInt8"),
            "nu16": pd.Series([6, None], dtype="UInt16"),
            "nu32": pd.Series([7, None], dtype="UInt32"),
            "nu64": pd.Series([8, None], dtype="UInt64"),
            "f32": pd.Series([1.25, np.nan], dtype="float32"),
            "f64": pd.Series([-2.5, np.inf], dtype="float64"),
            "nullable_f32": pd.Series([3.5, None], dtype="Float32"),
            "nullable_f64": pd.Series([4.5, None], dtype="Float64"),
            "native_bool": pd.Series([True, False], dtype="bool"),
            "boolean": pd.Series([True, None], dtype="boolean"),
        }
    )
    with bb.db(frames=128) as database:
        database.load_df(frame, "all_numeric")
        result = database.sql("SELECT * FROM all_numeric ORDER BY _id")

    assert result.types == [
        "BIGINT",
        "TINYINT",
        "SMALLINT",
        "INTEGER",
        "BIGINT",
        "UTINYINT",
        "USMALLINT",
        "UINTEGER",
        "UBIGINT",
        "TINYINT",
        "SMALLINT",
        "INTEGER",
        "BIGINT",
        "UTINYINT",
        "USMALLINT",
        "UINTEGER",
        "UBIGINT",
        "FLOAT",
        "DOUBLE",
        "FLOAT",
        "DOUBLE",
        "BOOLEAN",
        "BOOLEAN",
    ]
    first, second = result.tuples()
    assert first[1:9] == (-8, -16, -32, -64, 8, 16, 32, 2**63)
    assert second[9:17] == (None,) * 8
    assert second[17] is None  # NaN policy: SQL NULL.
    assert second[18] == np.inf  # Infinity policy: preserve IEEE infinity.
    assert second[19] is None
    assert second[20] is None
    assert second[21] is False
    assert second[22] is None


def test_strings_dates_timestamps_decimals_and_missing_values() -> None:
    frame = pd.DataFrame(
        {
            "text": pd.Series(["Grüezi 🐝", None, "long-" + "x" * 200], dtype="string"),
            "date_value": pd.Series([date(1969, 12, 31), None, date(2024, 2, 29)], dtype=object),
            "timestamp_value": pd.Series(
                [datetime(1969, 12, 31, 23, 59, 59, 123456), None, datetime(2024, 2, 29, 12, 0, 0, 1)],
                dtype=object,
            ),
            "native_timestamp": pd.to_datetime(
                ["1969-12-31 23:59:59.000001", None, "2024-02-29"], format="mixed"
            ),
            "amount": pd.Series([Decimal("1.20"), None, Decimal("-9999999999999.999")], dtype=object),
            "all_null": pd.Series([None, pd.NA, None], dtype=object),
        }
    )
    with bb.db(frames=128) as database:
        database.load_df(frame, "rich_types")
        rows = database.sql("SELECT * FROM rich_types ORDER BY _id").tuples()

    assert rows[0][1:] == (
        "Grüezi 🐝",
        date(1969, 12, 31),
        datetime(1969, 12, 31, 23, 59, 59, 123456),
        datetime(1969, 12, 31, 23, 59, 59, 1),
        Decimal("1.200"),
        None,
    )
    assert rows[1][1:] == (None, None, None, None, None, None)
    assert rows[2][2] == date(2024, 2, 29)
    assert rows[2][5] == Decimal("-9999999999999.999")


def test_categorical_string_integer_and_ordered_values() -> None:
    frame = pd.DataFrame(
        {
            "label": pd.Series(pd.Categorical(["b", "a", None], ordered=True)),
            "number": pd.Series(pd.Categorical([3, 2, 1], ordered=False)),
        }
    )
    with bb.db(frames=64) as database:
        database.load_df(frame, "categories")
        result = database.sql("SELECT _id, label, number FROM categories ORDER BY _id")
    assert result.types == ["BIGINT", "VARCHAR", "BIGINT"]
    assert result.tuples() == [(0, "b", 3), (1, "a", 2), (2, None, 1)]


def test_timezone_and_unsupported_mixed_object_values_fail_before_create() -> None:
    with bb.db(frames=64) as database:
        aware = pd.DataFrame({"when": pd.to_datetime(["2024-01-01T00:00:00Z"])})
        with pytest.raises(bb.DataError, match="timezone"):
            database.load_df(aware, "aware")

        mixed = pd.DataFrame({"value": pd.Series([1, "two"], dtype=object)})
        with pytest.raises(bb.DataError, match="mixed"):
            database.load_df(mixed, "mixed")

        too_wide = pd.DataFrame({"value": pd.Series([Decimal("1234567890123456789")], dtype=object)})
        with pytest.raises(bb.DataError, match="18-digit"):
            database.load_df(too_wide, "too_wide")

        for table in ("aware", "mixed", "too_wide"):
            with pytest.raises(bb.BinderError):
                database.sql(f"SELECT * FROM {table}")


def test_one_row_and_many_vector_dataframe() -> None:
    with bb.db(frames=128) as database:
        database.load_df(pd.DataFrame({"value": [42]}), "one_row")
        assert database.sql("SELECT value FROM one_row").tuples() == [(42,)]

        rows = 5_000
        database.load_df(
            pd.DataFrame({"id": np.arange(rows, dtype=np.int64), "value": np.arange(rows, dtype=np.float64)}),
            "many_vectors",
            primary_key="id",
        )
        assert database.sql("SELECT count(*), sum(id), sum(value) FROM many_vectors").tuples() == [
            (rows, rows * (rows - 1) // 2, float(rows * (rows - 1) // 2)),
        ]
