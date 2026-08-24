"""Create an in-memory database and consume owning results."""

import bumblebeedb as bb


with bb.db() as database:
    database.sql("CREATE TABLE readings(sensor_id INT PRIMARY KEY, value DOUBLE)")
    database.sql("INSERT INTO readings VALUES (1, 18.5), (2, 21.25), (3, NULL)")

    result = database.sql("SELECT sensor_id, value FROM readings ORDER BY sensor_id")
    print(result.columns, result.types)
    print(result.tuples())
    print(result.to_df())
