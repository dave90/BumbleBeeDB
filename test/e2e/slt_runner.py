"""Parser and driver for BumbleBeeDB sqllogictest-style (`.slt`) end-to-end tests.

Each `.slt` file is a sequence of records separated by blank lines, modeled on the
SQLite/bustub sqllogictest format:

    statement ok            # SQL that must succeed
    CREATE TABLE t(a INT);

    statement error         # SQL that must raise an error
    SELECT missing FROM t;

    query [rowsort]         # SQL whose result is compared against the block after ----
    SELECT a FROM t;
    ----
    1
    2

    sleep 300               # real pause in ms (transaction-timeout tests)

A ``statement`` / ``query`` directive may carry a *connection label* (``statement ok s1``,
``query rowsort s2``): the record then runs in that named shell session (``\session`` is emitted
on change), letting several sessions hold concurrent open transactions deterministically.

Lines beginning with ``#`` are comments. A few ``#`` comments are also *directives*
that configure the whole file (they may appear anywhere, but conventionally sit in a
header block):

    # config: morsel_pages=4 max_memory=65536 threads=1 prefer_external=true
    # seed: mock            # seed the demo tables before running (default: empty catalog)
    # require: small_vector # skip unless run against the small-vector (STANDARD_VECTOR_SIZE) build

The driver runs every record against a single long-lived ``BumbleBee`` process (so state
persists across records within a file) launched in ``--test-protocol`` mode. That mode
frames each statement's output with an ASCII record-separator status line and lets us
delimit a record's output with an ``\\echo`` sentinel.
"""

from __future__ import annotations

import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

# Must match kStatusMarker in src/main/main.cpp.
STATUS_MARKER = "\x1e"
# How BumbleBee renders a NULL Value (src/type/value.cpp). Expected blocks use this literal.
NULL_TOKEN = "NULL"

# `# config:` keys that map to a BumbleBee CLI flag. Only knobs with an OBSERVABLE effect through the
# in-memory shell are exposed, so a `.slt` cannot set something that silently does nothing. Unknown keys
# are a hard error (typo protection). Deliberately omitted: `morsel_size` (no consumer until the columnar
# scan lands) and `frames` (the harness always runs in-memory, which ignores it).
_CONFIG_FLAGS = {
    "max_memory": "--max-memory",
    "morsel_pages": "--morsel-pages",
    "threads": "--threads",
    # Transaction timeout in MILLISECONDS, enforced when a record runs `\gc`. Runtime-configurable
    # (unlike the compile-time vector size), so a timeout test needs no dedicated build.
    "txn_timeout": "--txn-timeout",
}
# Boolean config keys map to a bare flag when truthy.
_CONFIG_BOOL_FLAGS = {
    "prefer_external": "--prefer-external",
}


class SltError(Exception):
    """A parse or execution failure, carrying a human-readable, pytest-friendly message."""


@dataclass
class Record:
    """One executable `.slt` record."""

    kind: str  # "statement", "query" or "sleep"
    sql: str
    line: int  # 1-based line where the record's directive appears (for error messages)
    expect_error: bool = False  # statement error
    sort: bool = False  # query rowsort
    expected: list[str] = field(default_factory=list)  # query expected rows
    # Which named shell session the record runs in (sqllogictest connection label: the optional
    # last token of `statement ok s1` / `query rowsort s2`). The runner emits `\session <name>`
    # when this differs from the previous record's session, so several sessions — each with its
    # own open transaction — can be interleaved deterministically within one file.
    session: str = "default"
    sleep_ms: int = 0  # kind == "sleep": how long to pause before the next record


@dataclass
class SltFile:
    """A parsed `.slt` file: its directives and its ordered records."""

    path: Path
    config: dict[str, str] = field(default_factory=dict)
    seed_mock: bool = False
    requires: set[str] = field(default_factory=set)
    records: list[Record] = field(default_factory=list)

    def cli_flags(self) -> list[str]:
        """Translate the file's `# config:` block into BumbleBee CLI flags."""
        flags: list[str] = []
        for key, value in self.config.items():
            if key in _CONFIG_FLAGS:
                flags += [_CONFIG_FLAGS[key], value]
            elif key in _CONFIG_BOOL_FLAGS:
                if value.lower() in ("1", "true", "yes", "on"):
                    flags.append(_CONFIG_BOOL_FLAGS[key])
            else:
                raise SltError(f"unknown # config key: {key!r}")
        return flags


def _parse_directive(comment: str, out: SltFile) -> None:
    """Interpret a `#`-comment as a `config:` / `seed:` / `require:` directive, if it is one."""
    body = comment.lstrip("#").strip()
    if body.startswith("config:"):
        for token in body[len("config:"):].split():
            if "=" not in token:
                raise SltError(f"malformed # config token (want key=value): {token!r}")
            key, _, value = token.partition("=")
            out.config[key.strip()] = value.strip()
    elif body.startswith("seed:"):
        out.seed_mock = body[len("seed:"):].strip() == "mock"
    elif body.startswith("require:"):
        out.requires.update(body[len("require:"):].split())


def parse_slt(path: Path) -> SltFile:
    """Parse a `.slt` file into an :class:`SltFile`."""
    result = SltFile(path=path)
    lines = path.read_text().splitlines()
    i = 0
    n = len(lines)
    while i < n:
        raw = lines[i]
        stripped = raw.strip()
        # Blank lines separate records; comments carry directives.
        if stripped == "":
            i += 1
            continue
        if stripped.startswith("#"):
            _parse_directive(stripped, result)
            i += 1
            continue

        tokens = stripped.split()
        head = tokens[0]
        if head == "halt":
            break
        if head == "sleep":
            # `sleep <ms>`: a real pause, in milliseconds — used by the transaction-timeout tests to
            # outlive a `# config: txn_timeout=<ms>` before triggering `\gc`.
            if len(tokens) != 2 or not tokens[1].isdigit():
                raise SltError(f"line {i + 1}: 'sleep' wants a duration in ms: sleep 300")
            result.records.append(Record(kind="sleep", sql="", line=i + 1, sleep_ms=int(tokens[1])))
            i += 1
            continue

        if head == "statement":
            if len(tokens) < 2 or tokens[1] not in ("ok", "error"):
                raise SltError(f"line {i + 1}: 'statement' must be 'ok' or 'error'")
            expect_error = tokens[1] == "error"
            session = tokens[2] if len(tokens) > 2 else "default"
            sql_lines, i = _consume_sql(lines, i + 1)
            result.records.append(
                Record(kind="statement", sql="\n".join(sql_lines), line=i, expect_error=expect_error,
                       session=session)
            )
            continue

        if head == "query":
            sort = "rowsort" in tokens[1:]
            # Any modifier token that is not `rowsort` is a connection label (sqllogictest style).
            labels = [t for t in tokens[1:] if t != "rowsort"]
            if len(labels) > 1:
                raise SltError(f"line {i + 1}: at most one connection label per query record")
            session = labels[0] if labels else "default"
            directive_line = i + 1
            sql_lines: list[str] = []
            i += 1
            while i < n and lines[i].strip() != "----" and lines[i].strip() != "":
                if not lines[i].strip().startswith("#"):
                    sql_lines.append(lines[i])
                i += 1
            if i >= n or lines[i].strip() != "----":
                raise SltError(f"line {directive_line}: query record has no '----' result separator")
            i += 1  # skip ----
            expected: list[str] = []
            while i < n and lines[i].strip() != "":
                expected.append(lines[i].rstrip())
                i += 1
            result.records.append(
                Record(
                    kind="query",
                    sql="\n".join(sql_lines),
                    line=directive_line,
                    sort=sort,
                    expected=[e for e in expected if e.strip() != ""],
                    session=session,
                )
            )
            continue

        raise SltError(f"line {i + 1}: unrecognized directive: {stripped!r}")

    return result


def _consume_sql(lines: list[str], start: int) -> tuple[list[str], int]:
    """Collect SQL lines from `start` up to the next blank line; return (lines, next_index)."""
    sql_lines: list[str] = []
    i = start
    while i < len(lines) and lines[i].strip() != "":
        if not lines[i].strip().startswith("#"):
            sql_lines.append(lines[i])
        i += 1
    return sql_lines, i


class BumbleBeeSession:
    """Drives a single long-lived ``BumbleBee --test-protocol`` process for one `.slt` file."""

    def __init__(self, binary: str, flags: list[str], seed_mock: bool):
        argv = [binary, "--memory", "--test-protocol", *flags]
        if not seed_mock:
            argv.append("--no-seed")
        self._proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,  # line-buffered
        )
        self._token = 0
        # The shell starts in session "default"; a record with a different connection label makes
        # run_record emit a `\session` switch (which produces no output) before its SQL.
        self._session = "default"
        # Sync past any startup output before the first record.
        self._exec("\\echo __READY__", token_only=True)

    def _exec(self, payload: str, token_only: bool = False) -> tuple[list[str], list[str]]:
        """Send `payload`, then an echo sentinel; return (status_lines, output_lines) up to the sentinel."""
        assert self._proc.stdin is not None and self._proc.stdout is not None
        self._token += 1
        token = f"__SLT_{self._token}__"
        self._proc.stdin.write(payload + "\n")
        self._proc.stdin.write(f"\\echo {token}\n")
        self._proc.stdin.flush()

        status_lines: list[str] = []
        output_lines: list[str] = []
        while True:
            line = self._proc.stdout.readline()
            if line == "":  # EOF: the process died mid-record
                err = self._proc.stderr.read() if self._proc.stderr else ""
                raise SltError(f"BumbleBee process exited unexpectedly. stderr:\n{err}")
            line = line.rstrip("\n")
            if line == token:
                break
            if token_only:
                continue
            if line.startswith(STATUS_MARKER):
                status_lines.append(line[len(STATUS_MARKER):])
            else:
                output_lines.append(line)
        return status_lines, output_lines

    def run_record(self, record: Record) -> tuple[bool, str, list[str]]:
        """Execute one record; return (succeeded, error_message, result_rows)."""
        sql = record.sql.strip()
        # Meta-commands (`\gc`, ...) are single whole-line statements: no terminating semicolon.
        if not sql.endswith(";") and not sql.startswith("\\"):
            sql += ";"
        # Switch the shell to the record's session first, when it differs. `\session` is silent, so
        # it contributes only an "ok" status line, never output rows.
        if record.session != self._session:
            sql = f"\\session {record.session}\n" + sql
            self._session = record.session
        status, output = self._exec(sql)
        errored = any(s.startswith("err") for s in status)
        err_msg = next((s[len("err"):].strip() for s in status if s.startswith("err")), "")
        rows = [line.rstrip() for line in output if line.strip() != ""]
        return (not errored, err_msg, rows)

    def close(self) -> None:
        try:
            if self._proc.stdin is not None:
                self._proc.stdin.close()
            self._proc.wait(timeout=10)
        except Exception:
            self._proc.kill()


def _compare(actual: list[str], expected: list[str], sort: bool) -> None:
    """Compare a query's actual rows against the expected block, mirroring bustub's ResultCompare."""
    a = [x for x in actual if x.strip() != ""]
    e = [x for x in expected if x.strip() != ""]
    if sort:
        a = sorted(a)
        e = sorted(e)
    if a != e:
        raise SltError(
            "query result mismatch:\n"
            f"  expected ({len(e)} rows):\n"
            + "".join(f"    | {r}\n" for r in e)
            + f"  actual ({len(a)} rows):\n"
            + "".join(f"    | {r}\n" for r in a)
        )


def run_slt_file(path: Path, binary: str, is_small_vector_build: bool) -> None:
    """Parse and execute a `.slt` file end to end; raise :class:`SltError` on any failure.

    Returns None on success. Skips (via SltSkip) files whose `# require:` is unmet.
    """
    slt = parse_slt(path)

    if "small_vector" in slt.requires and not is_small_vector_build:
        raise SltSkip("requires the small-vector build (set BBDB_SLT_SMALL_VECTOR=1)")

    session = BumbleBeeSession(binary, slt.cli_flags(), slt.seed_mock)
    try:
        for record in slt.records:
            if record.kind == "sleep":
                time.sleep(record.sleep_ms / 1000.0)
                continue
            ok, err_msg, rows = session.run_record(record)
            loc = f"{path.name}:{record.line}"
            if record.kind == "statement":
                if record.expect_error and ok:
                    raise SltError(f"{loc}: statement expected an error but succeeded:\n  {record.sql}")
                if not record.expect_error and not ok:
                    raise SltError(f"{loc}: statement failed unexpectedly:\n  {record.sql}\n  error: {err_msg}")
            else:  # query
                if not ok:
                    raise SltError(f"{loc}: query failed unexpectedly:\n  {record.sql}\n  error: {err_msg}")
                try:
                    _compare(rows, record.expected, record.sort)
                except SltError as exc:
                    raise SltError(f"{loc}: {exc}\n  SQL: {record.sql}") from None
    finally:
        session.close()


class SltSkip(Exception):
    """Raised to signal a `.slt` file should be skipped (unmet `# require:`)."""
