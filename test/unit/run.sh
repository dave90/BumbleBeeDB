#!/usr/bin/env bash

set -euo pipefail

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=address
cmake --build build-debug --parallel 4
cd build-debug && ctest --output-on-failure
cd ..

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=thread
cmake --build build-tsan --parallel 4
cd build-tsan && ctest --output-on-failure
