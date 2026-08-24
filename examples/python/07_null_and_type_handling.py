"""Round-trip nullable and exact Python/pandas values."""

from datetime import date, datetime
from decimal import Decimal

import pandas as pd

import bumblebeedb as bb


frame = pd.DataFrame(
    {
        "nullable_int": pd.Series([1, None], dtype="Int64"),
        "nullable_bool": pd.Series([True, None], dtype="boolean"),
        "day": [date(2026, 8, 21), None],
        "moment": [datetime(2026, 8, 21, 12, 30), None],
        "amount": [Decimal("12.34"), None],
        "label": ["Grüezi 🐝", None],
    }
)

with bb.db() as database:
    database.load_df(frame, "typed_values")
    result = database.sql(
        "SELECT _id AS _id, nullable_int AS nullable_int, nullable_bool AS nullable_bool, "
        "day AS day, moment AS moment, amount AS amount, label AS label "
        "FROM typed_values ORDER BY _id"
    )
    print(result.tuples())
    round_trip = result.to_df()
    print(round_trip.dtypes)
    assert round_trip["nullable_int"].dtype == "Int64"
    assert round_trip["nullable_bool"].dtype == "boolean"
