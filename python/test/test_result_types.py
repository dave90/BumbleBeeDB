"""Complete Python scalar and DataFrame result conversion coverage."""

from __future__ import annotations

from datetime import date, datetime
from decimal import Decimal

import numpy as np
import pandas as pd
import pytest

import bumblebeedb as bb


def test_boolean_signed_and_unsigned_integer_boundaries() -> None:
    with bb.db(frames=64) as database:
        result = database.sql(
            "SELECT "
            "CAST(1 AS BOOLEAN), CAST(0 AS BOOLEAN), CAST(NULL AS BOOLEAN), "
            "CAST(-128 AS TINYINT), CAST(-32768 AS SMALLINT), "
            "CAST(-2147483648 AS INTEGER), CAST('-9223372036854775808' AS BIGINT), "
            "CAST(255 AS UTINYINT), CAST(65535 AS USMALLINT), "
            "CAST('4294967295' AS UINTEGER), CAST('18446744073709551615' AS UBIGINT)"
        )

        assert result.tuples() == [
            (
                True,
                False,
                None,
                -128,
                -32768,
                -2147483648,
                -9223372036854775808,
                255,
                65535,
                4294967295,
                18446744073709551615,
            )
        ]

        frame = result.to_df()
        assert list(map(str, frame.dtypes)) == [
            "bool",
            "bool",
            "boolean",
            "int8",
            "int16",
            "int32",
            "int64",
            "uint8",
            "uint16",
            "uint32",
            "uint64",
        ]
        assert frame.iloc[0, 10] == 18446744073709551615
        assert pd.isna(frame.iloc[0, 2])


def test_nullable_integer_dataframe_dtypes() -> None:
    with bb.db(frames=64) as database:
        result = database.sql(
            "VALUES "
            "(CAST(1 AS TINYINT), CAST(2 AS SMALLINT), CAST(3 AS INTEGER), CAST(4 AS BIGINT), "
            " CAST(5 AS UTINYINT), CAST(6 AS USMALLINT), CAST(7 AS UINTEGER), CAST(8 AS UBIGINT)), "
            "(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)"
        )
        frame = result.to_df()
        assert list(map(str, frame.dtypes)) == [
            "Int8",
            "Int16",
            "Int32",
            "Int64",
            "UInt8",
            "UInt16",
            "UInt32",
            "UInt64",
        ]
        assert frame.iloc[0].tolist() == [1, 2, 3, 4, 5, 6, 7, 8]
        assert all(pd.isna(value) for value in frame.iloc[1])


def test_float_decimal_date_and_timestamp_conversion() -> None:
    with bb.db(frames=64) as database:
        result = database.sql(
            "SELECT "
            "CAST(1.25 AS FLOAT), CAST(-2.5 AS DOUBLE), CAST(NULL AS FLOAT), "
            "CAST(123 AS DECIMAL(18,0)), CAST(-12.345 AS DECIMAL(18,3)), "
            "CAST(NULL AS DECIMAL(9,2)), "
            "CAST('1969-12-31' AS DATE), CAST('2024-02-29' AS DATE), CAST(NULL AS DATE), "
            "CAST('1969-12-31 23:59:59.123456' AS TIMESTAMP), "
            "CAST('2024-02-29 12:34:56.000001' AS TIMESTAMP), CAST(NULL AS TIMESTAMP)"
        )

        assert result.tuples() == [
            (
                1.25,
                -2.5,
                None,
                Decimal("123"),
                Decimal("-12.345"),
                None,
                date(1969, 12, 31),
                date(2024, 2, 29),
                None,
                datetime(1969, 12, 31, 23, 59, 59, 123456),
                datetime(2024, 2, 29, 12, 34, 56, 1),
                None,
            )
        ]

        frame = result.to_df()
        assert str(frame.dtypes.iloc[0]) == "float32"
        assert str(frame.dtypes.iloc[1]) == "float64"
        assert np.isnan(frame.iloc[0, 2])
        assert frame.iloc[0, 3] == Decimal("123")
        assert frame.iloc[0, 6] == date(1969, 12, 31)
        assert str(frame.dtypes.iloc[9]) == "datetime64[us]"
        assert pd.isna(frame.iloc[0, 11])


def test_strings_lists_arrays_and_nested_nulls() -> None:
    with bb.db(frames=64) as database:
        result = database.sql(
            "SELECT '', 'short', "
            "'a long out-of-line string with ''quote'' and \\ slash', "
            "'Grüezi 🐝', ARRAY[1, NULL, 3], "
            "CAST(ARRAY[4, NULL, 6] AS INT[3])"
        )
        assert result.tuples() == [
            (
                "",
                "short",
                "a long out-of-line string with 'quote' and \\ slash",
                "Grüezi 🐝",
                [1, None, 3],
                [4, None, 6],
            )
        ]
        assert result.types[-2:] == ["INTEGER[]", "INTEGER[3]"]

        frame = result.to_df()
        assert frame.iloc[0, 4] == [1, None, 3]
        assert frame.iloc[0, 5] == [4, None, 6]


def test_multichunk_conversion_is_repeatable_and_detached() -> None:
    with bb.db(frames=128) as database:
        database.sql("CREATE TABLE many_values(value INTEGER PRIMARY KEY, text VARCHAR)")
        database.sql(
            "INSERT INTO many_values VALUES "
            + ",".join(f"({value}, 'row-{value}')" for value in range(1500))
        )
        result = database.sql("SELECT value, text FROM many_values ORDER BY value")
        database.sql("DROP TABLE many_values")

    expected_edges = [(0, "row-0"), (1499, "row-1499")]
    first = result.tuples()
    second = result.tuples()
    assert len(first) == 1500
    assert [first[0], first[-1]] == expected_edges
    assert second == first

    frame_one = result.to_df()
    frame_two = result.to_df()
    pd.testing.assert_frame_equal(frame_one, frame_two)
    frame_one.iloc[0, 0] = 999
    assert frame_two.iloc[0, 0] == 0
    assert result.tuples()[0] == (0, "row-0")


def test_empty_typed_result_and_duplicate_columns() -> None:
    with bb.db(frames=64) as database:
        database.sql("CREATE TABLE empty_values(value INTEGER PRIMARY KEY)")
        result = database.sql("SELECT value AS duplicate, value AS duplicate FROM empty_values")
        assert result.columns == ["duplicate", "duplicate"]
        assert result.types == ["INTEGER", "INTEGER"]
        assert result.tuples() == []
        frame = result.to_df()
        assert list(frame.columns) == ["duplicate", "duplicate"]
        assert frame.empty


def test_sql_text_with_embedded_nul_is_rejected() -> None:
    with bb.db(frames=64) as database:
        with pytest.raises((bb.ParserError, ValueError)):
            database.sql("SELECT 'before\x00after'")
