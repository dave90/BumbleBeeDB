"""Close and reopen a durable database file."""

from pathlib import Path
from tempfile import TemporaryDirectory

import bumblebeedb as bb


with TemporaryDirectory(prefix="bumblebeedb-example-") as directory:
    path = Path(directory) / "analytics.bbdb"

    with bb.db(path) as database:
        database.sql("CREATE TABLE events(id INT PRIMARY KEY, label VARCHAR)")
        database.sql("INSERT INTO events VALUES (1, 'created'), (2, 'persisted')")

    with bb.db(path) as reopened:
        rows = reopened.sql("SELECT id, label FROM events ORDER BY id").tuples()
        assert rows == [(1, "created"), (2, "persisted")]
        print(rows)
