cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=address
cmake --build build-debug -j
cd build-debug && ctest --output-on-failure
cd ..

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZER=thread
cmake --build build-tsan -j
cd build-tsan && ctest --output-on-failure
