# Hashmap Mini Project

A C++ project comparing linear probing, separate chaining, `std::unordered_map`, and Sparsepp. The goal is to see how hash quality, memory locality, and branch prediction affect insertion and lookup.

The benchmarks use integer pairs and string keys with `Person*` values.

## Build and test

Requires a C++17 compiler, CMake 3.21+, Make, and Git. CMake fetches GoogleTest when needed.

```sh
cmake --preset release
cmake --build --preset release -j2
ctest --preset release
```

The 15 GoogleTest cases cover collisions, updates, resizing, full tables, and pointer ownership.

## Run benchmarks

Requires Python 3.9+:

```sh
python3 scripts/run_benchmarks.py --count 200000 --repeats 5
```

Add `--operation find-hit` or `--operation find-miss` for lookups, or `--mode growing` to include resizing. New results go in `results/`.

The custom maps are in [maps.hpp](include/hashbench/maps.hpp), and the benchmark is in [benchmark.cpp](src/benchmark.cpp).

See the [report](docs/REPORT.md), [testing method](docs/METHODOLOGY.md), and [saved data](docs/data/README.md) for the findings.
