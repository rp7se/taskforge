# TaskForge benchmark harness

`taskforge_bench` is a dependency-free C++20 evaluation executable. It uses
`std::chrono::steady_clock`, existing TaskForge APIs, fixed workloads, and
explicit invariant checks. It is not a production scheduler feature.

Build and run the complete Release evaluation:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./bench/run_benchmarks.sh build-release docs/benchmark-results.md
```

The executable supports `--format text` and `--format json`; use `--help` for
`--scenario`, `--warmup`, `--iterations`, `--tasks`, and `--workers`. JSON is
hand-generated from numeric and fixed string fields, with no parser/library
dependency.

Scenarios cover launch latency, worker scaling and fixed-worker concurrency,
CPU and memory logical admission, bounded queue backpressure, bounded output
capture, and deterministic mixed-workload completion accounting. Timing has no
pass/fail threshold: structural invariants are the smoke-test gate.
