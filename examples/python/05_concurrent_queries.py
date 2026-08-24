"""Share one Database across Python callers while native workers obey one global cap."""

from concurrent.futures import ThreadPoolExecutor

import bumblebeedb as bb


with bb.db(worker_threads=4) as database:
    database.sql("CREATE TABLE events(value INT)")

    # Eight Python callers may overlap. worker_threads=4 is the database-wide native execution
    # budget, not the size of this Python pool and not a per-query worker count.
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(lambda value: database.sql(f"INSERT INTO events VALUES ({value})"), range(40)))
        counts = list(
            pool.map(
                lambda bucket: database.sql(
                    f"SELECT COUNT(*) FROM events WHERE value >= {bucket * 5} "
                    f"AND value < {(bucket + 1) * 5}"
                ).tuples()[0][0],
                range(8),
            )
        )

    assert sum(counts) == 40
    identifiers = [row[0] for row in database.sql("SELECT _id FROM events ORDER BY _id").tuples()]
    assert len(identifiers) == len(set(identifiers)) == 40
    print(counts)
