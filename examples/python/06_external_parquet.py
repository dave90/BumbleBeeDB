"""Create, query, rewrite, explain, and vacuum an external parquet table."""

from pathlib import Path
from tempfile import TemporaryDirectory

import bumblebeedb as bb


with TemporaryDirectory(prefix="bumblebeedb-parquet-") as directory:
    location = str(Path(directory) / "events").replace("'", "''")
    with bb.db() as database:
        database.sql(
            f"CREATE TABLE events(id INT, payload VARCHAR) "
            f"WITH (format='parquet', location='{location}')"
        )
        database.sql("INSERT INTO events VALUES (1, 'cold'), (2, 'hot'), (100, 'archive')")
        database.sql("UPDATE events SET payload = 'warm' WHERE id = 1")

        plan = database.explain("SELECT payload FROM events WHERE id > 50", mode="physical")
        print(plan)  # The physical scan carries the filter used for parquet row-group pruning.
        print(database.sql("SELECT * FROM events ORDER BY id").tuples())
        print("vacuum removed", database.vacuum("events"), "superseded file(s)")
