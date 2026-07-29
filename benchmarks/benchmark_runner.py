#!/usr/bin/env python3
# ===----------------------------------------------------------------------===
#
#                         BumbleBeeDB
#
# benchmark_runner.py
#
# Identification: benchmarks/benchmark_runner.py
#
# ===----------------------------------------------------------------------===
"""Config-driven benchmark harness for BumbleBeeDB.

Ported and adapted from the BumbleBee (datalog) benchmark runner. Each engine is
described by one JSON config (see ``configs/``). A config lists an untimed
``setup`` phase (schema / external-table or view declarations, DML seed) followed
by a set of ``tests``; every test is one SQL statement, run ``NUM_TRIES`` times,
timed by wall-clock. Results land in a timestamped CSV under ``results/`` with the
average/min/max, the delta vs a *comparison* engine (DuckDB for OLAP, SQLite for
DML) and the delta vs this config's previous run (regression tracking).

Two BumbleBeeDB-specific adaptations vs the datalog original:

* The shell has no ``-i <file>`` flag and no ``COPY ... TO``. Single-statement
  tests are driven with ``-c "$(cat <file>)"``; multi-statement scripts (setup,
  transaction workloads) are piped to stdin. Correctness is checked by comparing
  *normalized stdout* against the comparison engine, not exported CSV files.
* Some SQL features are not implemented yet (AVG, IN, LIKE, scalar subqueries,
  ...). Tests that need them carry an ``"xfail": "<reason>"`` marker: the harness
  still runs and times them, but an error is expected, not a regression. If an
  xfail test unexpectedly *succeeds*, the row is flagged ``xfail-PASS`` so we know
  the feature has landed and the marker can be dropped.
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from glob import glob
from pathlib import Path
from statistics import mean

HERE = Path(__file__).resolve().parent
RESULTS_FOLDER = HERE / "results"
NUM_TRIES = 3
TIMEOUT = 300

# The ASCII record-separator the shell's --test-protocol prefixes to each per-statement
# status line ("<RS>ok" / "<RS>err <message>"). We strip these when normalizing output.
RS = "\x1e"


def _expand(template: str, **kv) -> str:
    """Substitute ``$NAME`` / ``${NAME}`` placeholders in a command template."""
    out = template
    for key, val in kv.items():
        out = out.replace("${" + key + "}", str(val)).replace("$" + key, str(val))
    return out


def run_capture(command: str, timeout: int) -> tuple[float, str, int]:
    """Run ``command`` in a shell, returning ``(elapsed_seconds, combined_output, returncode)``.

    A timeout is not fatal: it counts as ``TIMEOUT`` seconds so a single slow query
    does not abort the whole sweep.
    """
    start = time.perf_counter()
    try:
        proc = subprocess.run(command, shell=True, capture_output=True, text=True, timeout=timeout)
        return time.perf_counter() - start, (proc.stdout or "") + (proc.stderr or ""), proc.returncode
    except subprocess.TimeoutExpired as exc:
        partial = (exc.stdout or "") + (exc.stderr or "") if isinstance(exc.stdout, str) else ""
        return float(timeout), partial + "\n[ERROR] timed out\n", 124


FLOAT_SIGFIGS = 10  # cross-engine float comparison tolerance, in significant figures


def _norm_token(tok: str) -> str:
    """Canonicalize one whitespace-delimited field so the two engines agree on it.

    Integers are kept exact; anything that parses as a float *and* looks fractional
    (has a '.' or exponent) is rounded to ``FLOAT_SIGFIGS`` significant figures via ``%g``.
    Significant figures (not fixed decimals) are the right tolerance across magnitudes:
    BumbleBeeDB accumulates decimal sums in a double (~12 good digits) while DuckDB is exact,
    so a 2.7e11 revenue sum differs in the ~4th decimal — equal to 10 s.f., not to 2 decimals.
    Non-numeric tokens (strings, dates, URLs) pass through untouched.
    """
    try:
        value = float(tok)
    except ValueError:
        return tok
    if any(c in tok for c in ".eE") and tok.lower() not in ("inf", "-inf", "nan"):
        return f"{value:.{FLOAT_SIGFIGS}g}"
    return tok


def normalize(raw: str) -> list[str]:
    """Reduce engine stdout to a comparable, order-independent multiset of value rows.

    Beyond dropping status/blank lines, this bridges the two engines' formatting so a
    *logically equal* result compares equal: string quoting is stripped (BumbleBeeDB
    prints ``'x'``, DuckDB ``x``), whitespace is collapsed, and fractional numbers are
    rounded to ``FLOAT_DECIMALS``. Rows are sorted, so this checks the result *set*;
    ORDER BY sequence is intentionally not verified (engines break ties differently).
    """
    rows = []
    # Split on newlines only: str.splitlines() also breaks on the RS byte (0x1e), which would
    # detach the status word from its marker and leak it into the row set.
    for line in raw.split("\n"):
        is_status = RS in line  # --test-protocol prefixes status lines with the record separator
        line = line.replace(RS, "").strip()
        if not line or is_status:
            continue
        if line.startswith(("[ERROR]", "Error:", "Parser Error", "Binder Error")):
            continue
        line = line.replace("'", "")  # BumbleBeeDB wraps strings in single quotes; DuckDB doesn't
        rows.append(" ".join(_norm_token(t) for t in line.split()))
    return sorted(rows)


FLOAT_RTOL = 1e-6  # relative tolerance for cross-engine float comparison


def _tokens_match(a: str, b: str) -> bool:
    """Two fields agree if identical, or — when both are numeric — within ``FLOAT_RTOL`` relative.

    A parallel engine reduces a SUM by merging per-worker partial sums in a nondeterministic order,
    and floating-point addition is not associative, so a large revenue sum can differ from an exact
    engine in the last few ULPs. A relative tolerance accepts that last-digit noise (mq07/q05-style
    revenue) while still catching a genuinely wrong value.
    """
    if a == b:
        return True
    try:
        fa, fb = float(a), float(b)
    except ValueError:
        return False
    return abs(fa - fb) <= FLOAT_RTOL * max(abs(fa), abs(fb), 1.0)


def rows_match(actual: list[str], expected: list[str]) -> bool:
    """Compare two normalized, sorted result sets field-by-field with a float tolerance."""
    if len(actual) != len(expected):
        return False
    for actual_row, expected_row in zip(actual, expected):
        actual_fields, expected_fields = actual_row.split(), expected_row.split()
        if len(actual_fields) != len(expected_fields):
            return False
        if not all(_tokens_match(x, y) for x, y in zip(actual_fields, expected_fields)):
            return False
    return True


def is_error(raw: str, returncode: int) -> bool:
    """Did the statement fail? True on non-zero exit or a ``<RS>err`` status marker."""
    if returncode not in (0, None):
        return True
    return (RS + "err") in raw or "[ERROR] timed out" in raw


def run_setup(config: dict) -> None:
    """Run the untimed setup phase: (re)create the engine's database and declare tables."""
    db = config.get("db", "")
    if db and db not in (":memory:", ""):
        Path(db).parent.mkdir(parents=True, exist_ok=True)
    if config.get("reset_db") and db and db not in (":memory:", ""):
        for path in glob(db + "*"):  # bb.db plus any sidecar files
            Path(path).unlink(missing_ok=True)
    for script in config.get("setup", []):
        script_path = HERE / script
        cmd = _expand(config["setup_cmd"], DB=db, FILE=script_path)
        print(f"[setup] {cmd}")
        elapsed, out, rc = run_capture(cmd, TIMEOUT)
        if is_error(out, rc):
            print(f"  [WARN] setup reported an error:\n{out[-800:]}")
        else:
            print(f"  [ok] {elapsed:.2f}s")


def run_test(test: dict, config: dict, expected_dir: Path, save_expected: bool):
    """Run one test ``NUM_TRIES`` times; return a result row dict plus its normalized output."""
    name = test["test"]
    query_file = HERE / config["query_dir"] / test["file"]
    driver = test.get("driver", config.get("driver", "run"))
    cmd_template = config["stdin_cmd"] if driver == "stdin" else config["run_cmd"]
    cmd = _expand(cmd_template, DB=config.get("db", ""), FILE=query_file)
    timeout = test.get("timeout", TIMEOUT)

    times, last_out, last_rc = [], "", 0
    for _ in range(test.get("num_tries", NUM_TRIES)):
        elapsed, last_out, last_rc = run_capture(cmd, timeout)
        times.append(elapsed)

    errored = is_error(last_out, last_rc)
    rows = normalize(last_out)
    xfail = test.get("xfail")

    # Correctness: the reference engine saves its normalized rows; the engine under
    # test compares against them.
    if save_expected:
        expected_dir.mkdir(parents=True, exist_ok=True)
        (expected_dir / f"{name}.out").write_text("\n".join(rows))

    if save_expected:
        match = "ref-error" if errored else "ref"
    elif xfail:
        match = "xfail" if errored else "xfail-PASS"
    elif errored:
        match = "ERROR"
    elif not config.get("compare", True):
        # DML statements return no comparable rows across engines (different status/count
        # formats); correctness is meaningless here, only the timing matters.
        match = "-"
    else:
        exp_file = expected_dir / f"{name}.out"
        if not exp_file.exists():
            match = "-"
        else:
            match = "match" if rows_match(rows, exp_file.read_text().splitlines()) else "mismatch"

    return {
        "name": name,
        "file": test["file"],
        "avg": mean(times),
        "min": min(times),
        "max": max(times),
        "match": match,
        "xfail": xfail or "",
        "note": test.get("note", ""),
        "error_tail": "" if not errored else re.sub(r"\s+", " ", last_out)[-160:],
    }, rows


def latest_results(config_name: str) -> dict:
    """Load ``{test: avg}`` from the most recent CSV for ``config_name`` (for deltas)."""
    files = sorted(glob(str(RESULTS_FOLDER / f"{config_name}_*.csv")), reverse=True)
    if not files:
        return {}
    with open(files[0], newline="") as handle:
        return {r["test"]: float(r["avg"]) for r in csv.DictReader(handle) if r["avg"] not in ("", "-")}


def write_csv(config_name: str, rows: list[dict]) -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = RESULTS_FOLDER / f"{config_name}_{stamp}.csv"
    fields = ["test", "file", "avg", "min", "max", "delta_vs_cmp", "delta_vs_cmp_%",
              "delta_vs_prev", "match", "xfail", "note", "error_tail"]
    with open(out, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="BumbleBeeDB benchmark runner")
    parser.add_argument("config", help="path to a benchmark config JSON")
    parser.add_argument("--only", default="", help="comma-separated subset of test names to run")
    parser.add_argument("--skip-setup", action="store_true", help="reuse an existing database")
    args = parser.parse_args()

    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = HERE / config_path
    config = json.loads(config_path.read_text())
    config_name = config_path.stem
    RESULTS_FOLDER.mkdir(parents=True, exist_ok=True)

    # Where this engine's normalized outputs live, and where the comparison engine's do.
    expected_root = HERE / "results" / "_expected"
    my_expected = expected_root / config_name
    cmp_config = config.get("comparison_config")
    cmp_name = Path(cmp_config).stem if cmp_config else None
    cmp_expected = expected_root / cmp_name if cmp_name else my_expected
    # A config is a "reference" when nothing else compares against it (no comparison_config):
    # it saves outputs for others; engines under test compare against the reference's saved set.
    save_expected = cmp_config is None

    cmp_times = latest_results(cmp_name) if cmp_name else {}
    prev_times = latest_results(config_name) if config.get("previous_comparison") else {}

    if not args.skip_setup:
        run_setup(config)

    only = set(filter(None, args.only.split(",")))
    print(f"\n{'test':<22}{'avg(s)':>9}{'min':>9}{'vs cmp':>10}{'vs prev':>10}  match")
    print("-" * 78)

    results = []
    for test in config["tests"]:
        if only and test["test"] not in only:
            continue
        if test.get("skip"):
            continue
        row, _ = run_test(test, config, cmp_expected if not save_expected else my_expected, save_expected)

        cmp_avg = cmp_times.get(row["name"])
        prev_avg = prev_times.get(row["name"])
        d_cmp = f"{row['avg'] - cmp_avg:+.3f}" if cmp_avg is not None else "-"
        d_cmp_pct = f"{(row['avg'] - cmp_avg) / cmp_avg * 100:+.0f}%" if cmp_avg else "-"
        d_prev = f"{row['avg'] - prev_avg:+.3f}" if prev_avg is not None else "-"

        results.append({
            "test": row["name"], "file": row["file"],
            "avg": f"{row['avg']:.4f}", "min": f"{row['min']:.4f}", "max": f"{row['max']:.4f}",
            "delta_vs_cmp": d_cmp, "delta_vs_cmp_%": d_cmp_pct, "delta_vs_prev": d_prev,
            "match": row["match"], "xfail": row["xfail"], "note": row["note"],
            "error_tail": row["error_tail"],
        })
        flag = "" if not row["error_tail"] else "  <<< " + row["error_tail"][:80]
        print(f"{row['name']:<22}{row['avg']:>9.3f}{row['min']:>9.3f}"
              f"{d_cmp:>10}{d_prev:>10}  {row['match']}{flag}")

    print("-" * 78)
    csv_file = write_csv(config_name, results)
    print(f"\nSaved -> {csv_file.relative_to(HERE)}")


if __name__ == "__main__":
    main()
