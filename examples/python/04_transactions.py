"""Use independent Connections for commit, rollback, snapshots, and conflicts."""

import bumblebeedb as bb


with bb.db() as database:
    database.sql("CREATE TABLE accounts(id INT PRIMARY KEY, balance INT)")
    database.sql("INSERT INTO accounts VALUES (1, 100), (2, 100)")

    with database.connect() as transfer:
        with transfer.transaction():
            transfer.sql("UPDATE accounts SET balance = balance - 10 WHERE id = 1")
            transfer.sql("UPDATE accounts SET balance = balance + 10 WHERE id = 2")

        try:
            with transfer.transaction():
                transfer.sql("UPDATE accounts SET balance = 0")
                raise RuntimeError("cancel this application operation")
        except RuntimeError:
            pass

    with database.connect() as older, database.connect() as writer:
        older.begin()
        before = older.sql("SELECT balance FROM accounts WHERE id = 1").tuples()
        writer.sql("UPDATE accounts SET balance = balance + 1 WHERE id = 1")
        assert older.sql("SELECT balance FROM accounts WHERE id = 1").tuples() == before
        older.commit()

    with database.connect() as first, database.connect() as second:
        first.begin()
        second.begin()
        first.sql("UPDATE accounts SET balance = balance + 1 WHERE id = 2")
        first.commit()
        try:
            second.sql("UPDATE accounts SET balance = balance + 1 WHERE id = 2")
        except bb.ConflictError as error:
            print("expected write conflict:", error)

    print(database.sql("SELECT id, balance FROM accounts ORDER BY id").tuples())
