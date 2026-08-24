"""Copy pandas data into tables, then join and aggregate it natively."""

import pandas as pd

import bumblebeedb as bb


customers = pd.DataFrame({"customer_id": [1, 2], "segment": ["business", "personal"]})
sales = pd.DataFrame(
    {
        "sale_id": pd.Series([10, 11, 12], dtype="Int64"),
        "customer_id": pd.Series([1, 1, 2], dtype="Int64"),
        "amount": pd.Series([12.5, None, 8.0], dtype="Float64"),
    }
)

with bb.db() as database:
    database.load_df(customers, "customers", primary_key="customer_id")
    database.load_df(sales, "sales", primary_key="sale_id")
    result = database.sql(
        """
        SELECT c.segment, COUNT(*) AS sales, SUM(s.amount) AS total
        FROM customers c JOIN sales s ON c.customer_id = s.customer_id
        GROUP BY c.segment ORDER BY c.segment
        """
    )
    frame = result.to_df()
    print(frame)
    assert frame["sales"].tolist() == [2, 1]
