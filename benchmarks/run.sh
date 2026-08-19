cmake -S .. -B ../build -DCMAKE_BUILD_TYPE=Release && cmake --build ../build -j

python3 benchmark_runner.py configs/bumblebeedb_olap.json
python3 benchmark_runner.py configs/bumblebeedb_dml.json