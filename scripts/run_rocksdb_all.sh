#!/bin/bash
set -e
echo "Starting RocksDB benchmarks for 3M, 5M, 10M, 15M..."
./rocksdb_bench --scale 3000000 --repeats 3
./rocksdb_bench --scale 5000000 --repeats 3
./rocksdb_bench --scale 10000000 --repeats 3
./rocksdb_bench --scale 15000000 --repeats 3
echo "All RocksDB scales finished successfully!"
