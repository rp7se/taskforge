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

## Sanitizer / Hardening

Optional ASan+UBSan and TSan builds are maintained separately from the normal
build. See [the sanitizer hardening guide](docs/sanitizers.md) for local
commands, test scope, and the fork/exec caveat.

## Benchmark

Phase 9 adds a reproducible Release-only evaluation harness for launch latency,
worker scaling, logical resource admission, bounded-queue backpressure, bounded
output retention, and deterministic mixed-workload completion accounting. It
uses fixed workloads and correctness invariants; CI runs only short structural
smoke scenarios and has no timing threshold.

The following are local Release observations for the documented WSL2
environment and fixed workloads; they are not cross-machine performance claims
or CI timing gates.

| Scenario | Configuration | Result |
| --- | --- | --- |
| Process launch | 100 measured helper exit-zero runs | p50 21,314 us; p95 21,604 us |
| Worker throughput | 100 tasks; fixed 25 ms workload | 1/2/4/8 workers: 21.48/42.90/85.62/163.93 tasks/s |
| CPU admission | 4 workers; 2000m capacity; 1000m/task | observed max running 2 |
| Queue backpressure | 2 workers; queue capacity 8; burst 32 | accepted 10; queue_full 24; completed 10 |
| Bounded output | 16 MiB stdout + 16 MiB stderr generated/drained and accounted | stdout total 16,777,216 bytes; stderr total 16,777,216 bytes; stdout/stderr retained 65,536 bytes each |
| Mixed stress | 300 accepted deterministic tasks | completed 300: 200 success, 50 failed, 25 timed out, 25 cancelled; unique IDs |

The bounded-output row describes retained stdout/stderr payload, not TaskForge
RSS: each stream retained 64 KiB while 16 MiB per stream was drained and
accounted.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./bench/run_benchmarks.sh build-release docs/benchmark-results.md
```

See [the full benchmark environment, results, methodology, and limitations](docs/benchmark-results.md).
