"""Use explain modes, SQL scripts/files, and structured engine exceptions."""

from pathlib import Path
from tempfile import TemporaryDirectory

import bumblebeedb as bb


with bb.db() as database:
    results = database.execute_script(
        "CREATE TABLE metrics(v INT); INSERT INTO metrics VALUES (1), (2), (3);"
    )
    print([result.command_tag for result in results])
    for mode in ("binder", "planner", "optimizer", "physical", "pipelines", "analyze"):
        print(database.explain("SELECT SUM(v) FROM metrics", mode=mode))

    with TemporaryDirectory(prefix="bumblebeedb-script-") as directory:
        script = Path(directory) / "report.sql"
        script.write_text("SELECT COUNT(*) FROM metrics; SELECT MAX(v) FROM metrics;", encoding="utf-8")
        print([result.tuples() for result in database.run_file(script)])

    try:
        database.execute_script("SELECT 1; SELECT missing_column; SELECT 3")
    except bb.BinderError as error:
        print("structured script error:", error)  # Includes the one-based failing statement index.
