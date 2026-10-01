# TaskForge Phase 9 benchmark results

## Environment (sanitized)

- Date: 2026-10-01T22:27:14+08:00
- OS/kernel/architecture: Linux 6.18.33.2-microsoft-standard-WSL2 x86_64
- WSL: yes
- CPU model: AMD Ryzen 9 8945HX with Radeon Graphics
- Logical CPUs: 32
- Memory KiB: 16264932
- Compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- CMake: cmake version 3.28.3
- Build type: Release
- Git SHA: 8aaabf8062f0d916ef6cf559688737268cd8a4f2

No usernames, home paths, hostnames, tokens, or environment variables are recorded.

## Command

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./bench/run_benchmarks.sh build-release docs/benchmark-results.md
```

## Results

| Scenario | Configuration | Tasks / iterations | Elapsed ms | Throughput / latency | Observed invariant |
| --- | --- | ---: | ---: | --- | --- |
| Launch latency | helper exit 0; 10 warm-ups | 100 | 2133 | p50 21314 us; p95 21604 us | completed |
| Worker scaling (1) | 25 ms sleep; 1 worker | 100 | 4656 | 21.48 tasks/s | n/a |
| Worker scaling (2) | 25 ms sleep; 2 workers | 100 | 2331 | 42.90 tasks/s | n/a |
| Worker scaling (4) | 25 ms sleep; 4 workers | 100 | 1168 | 85.62 tasks/s | n/a |
| Worker scaling (8) | 25 ms sleep; 8 workers | 100 | 610 | 163.93 tasks/s | n/a |
| CPU admission | 4 workers; 2000m capacity; 1000m/task | 100 | 5093 | 19.63 tasks/s | max running 2; disabled control 39.23 tasks/s |
| Memory admission | 3 workers; 256 MiB capacity; 128 MiB/task | 100 | 5095 | 19.63 tasks/s | max running 2 |
| Queue backpressure | 2 workers; queue capacity 8; burst 32 | 10 accepted | 307 | 32.57 tasks/s | queue_full 24; completed 10 |
| Bounded output | 16 MiB stdout + 16 MiB stderr; 64 KiB/stream retained | 1 | 27 | retained 65536 + 65536 bytes | totals 16777216 + 16777216 bytes; truncated |
| Mixed stress | deterministic 12-case pattern | 300 accepted | 1562 | 192.06 tasks/s | completed 300; unique IDs 300 |

```text
scenario=launch elapsed_ms=2133 tasks_per_second=46.88
  iterations=100
  max_us=21938
  min_us=21033
  p50_us=21314
  p95_us=21604
  warmup=10
scenario=worker_scaling elapsed_ms=0 tasks_per_second=0.00
  sleep_ms=25
  tasks=100
  worker_1_elapsed_ms=4656
  worker_1_tasks_per_second=21.48
  worker_2_elapsed_ms=2331
  worker_2_tasks_per_second=42.90
  worker_4_elapsed_ms=1168
  worker_4_tasks_per_second=85.62
  worker_8_elapsed_ms=610
  worker_8_tasks_per_second=163.93
scenario=worker_concurrency elapsed_ms=263 tasks_per_second=30.42
  configured_workers=4
  invariant=true
  observed_max_running=4
scenario=resource_admission elapsed_ms=5093 tasks_per_second=19.63
  observed_max_running=2
  reserved_after_completion_cpu_millis=0
  reserved_after_completion_memory_bytes=0
  resource_cpu_capacity_millis=2000
  resource_disabled_elapsed_ms=2549
  resource_disabled_observed_max_running=4
  resource_disabled_tasks_per_second=39.23
  task_cpu_request_millis=1000
  worker_count=4
scenario=memory_admission elapsed_ms=5095 tasks_per_second=19.63
  memory_capacity_bytes=268435456
  observed_max_running=2
  task_memory_request_bytes=134217728
  worker_count=3
scenario=bounded_queue_backpressure elapsed_ms=307 tasks_per_second=32.57
  accepted=10
  burst_submissions=32
  completed=10
  queue_capacity=8
  queue_full=24
  worker_count=2
scenario=bounded_output_capture elapsed_ms=27 tasks_per_second=0.00
  stderr_retained_bytes=65536
  stderr_total_bytes=16777216
  stderr_truncated=true
  stdout_retained_bytes=65536
  stdout_total_bytes=16777216
  stdout_truncated=true
scenario=mixed_stress elapsed_ms=1562 tasks_per_second=192.06
  accepted=300
  accepted_id_count=300
  cancelled=25
  completed=300
  failed=50
  submitted=300
  success=200
  timeout=25
  unique_id_count=300
```

## Methodology and limitations

- Launch uses 10 warm-ups and 100 measured helper exit-zero processes; p95 is the nearest-rank percentile.
- Worker scaling uses the same 25 ms sleep workload and task count for each worker count.
- Resource scenarios validate declared logical admission budgets, not host CPU utilization or cgroup limits.
- Output capture retains the first 64 KiB per stream while draining and accounting for 16 MiB per stream.
- Mixed stress is deterministic and verifies accepted/completed equality and task-ID uniqueness.
- Measurements are local observations, not cross-machine performance claims or CI timing gates.
