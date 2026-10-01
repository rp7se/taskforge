# Taskforge

Taskforge is a C++20 project built with CMake.

## Current status

- Phase 1: Linux Process Executor
- Phase 2: thread-safe lifecycle state transitions
- Phase 3: timeout and C++20 stop-token cancellation for direct child processes
- Phase 4: process-group cleanup with SIGTERM grace and SIGKILL escalation
- Phase 5: Concurrent Executor and Backpressure
- Phase 6: cgroup v2 Resource Control (explicit delegated roots, per-task CPU,
  memory, and PID limits)
- Phase 7: Resource-Aware Admission (explicit CPU/memory reservations, atomic
  FIFO admission/release, and drain-safe bounded queue integration)
- Phase 8: Bounded Output Capture (independent stdout/stderr prefix limits,
  drained-byte accounting, and truncation diagnostics)

## Build and test

```sh
cmake -S . -B build
cmake --build build
./build/taskforge
ctest --test-dir build --output-on-failure
```

## Benchmark

Phase 9 adds a reproducible Release-only evaluation harness for launch latency,
worker scaling, logical resource admission, bounded-queue backpressure, bounded
output retention, and deterministic mixed-workload completion accounting. It
uses fixed workloads and correctness invariants; CI runs only short structural
smoke scenarios and has no timing threshold.

Latest local Release results (WSL2, Ryzen 9 8945HX, 32 logical CPUs) measured
launch p50/p95 of 21,314/21,604 us and 1/2/4/8-worker throughput of
21.48/42.90/85.62/163.93 tasks/s for the fixed 25 ms workload. Resource
admission limited the 2000m/1000m workload to two concurrent tasks; a 300-task
mixed stress run completed every accepted task with unique IDs.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./bench/run_benchmarks.sh build-release docs/benchmark-results.md
```

See [the full benchmark environment, results, methodology, and limitations](docs/benchmark-results.md).
