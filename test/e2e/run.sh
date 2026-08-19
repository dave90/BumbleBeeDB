#!/usr/bin/env bash
# Run the full e2e matrix: the .slt corpus in-memory and file-backed (durable), on both the
# normal and the small-vector build. Run from the repo root.
set -euo pipefail

# Normal build: in-memory, then file-backed (real disk IO + catalog persistence; unlocks the
# `# require: durable` files in persistence/, which restart the shell to assert reopen semantics).
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --target BumbleBee --parallel 4
BBDB_SLT_BIN=build-debug/BumbleBee python -m pytest test/e2e -v
BBDB_SLT_BIN=build-debug/BumbleBee BBDB_SLT_DURABLE=1 python -m pytest test/e2e -v

# Small-vector build (STANDARD_VECTOR_SIZE=4: multi-chunk paths on a handful of rows; unlocks the
# `# require: small_vector` files), again in both storage modes.
cmake -S . -B build-smallvec -DCMAKE_BUILD_TYPE=Release -DBBDB_VECTOR_SIZE=4
cmake --build build-smallvec --target BumbleBee --parallel 4
BBDB_SLT_BIN=build-smallvec/BumbleBee BBDB_SLT_SMALL_VECTOR=1 python -m pytest test/e2e -v
BBDB_SLT_BIN=build-smallvec/BumbleBee BBDB_SLT_SMALL_VECTOR=1 BBDB_SLT_DURABLE=1 python -m pytest test/e2e -v
